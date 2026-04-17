/**
 * @file test_proto_wrap.cpp
 * @brief ProtoCodec 跨环尾回绕正确性测试
 *
 * 使用小容量 RingChannel（4096 字节），通过填充帧推进 write_pos/read_pos
 * 到接近环尾，然后发送 protobuf 消息使帧跨越环尾回绕，验证解码正确性。
 *
 * 测试场景：
 * 1. 遍历多个 tail 偏移，写入 ring → Peek → 拼接 → DecodeHeader + ParseFromArray
 * 2. 跨进程 Send + FrameReader 经小容量 ring 收发跨环尾
 * 3. 连续多帧回绕，混合大小 protobuf
 */

#include <shm_ipc/proto_codec.hpp>
#include <shm_ipc/ringbuf.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shm_messages.pb.h"

namespace {

#define ASSERT(cond)                                                      \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n",                    \
                         __FILE__, __LINE__, #cond);                      \
            std::abort();                                                 \
        }                                                                 \
    } while (0)

constexpr std::size_t kSmallCap = 4096;
using SmallRing = shm::RingBuf<kSmallCap>;
using SmallChannel = shm::RingChannel<kSmallCap>;

shm_ipc::HeartbeatProto MakeHeartbeat(int32_t client_id, int32_t seq,
                                      int64_t ts)
{
    shm_ipc::HeartbeatProto hb;
    hb.set_client_id(client_id);
    hb.set_seq(seq);
    hb.set_timestamp(ts);
    return hb;
}

shm_ipc::ClientMsgProto MakeClientMsg(int32_t seq, int32_t tick,
                                      int64_t ts, uint32_t payload_size)
{
    shm_ipc::ClientMsgProto cm;
    cm.set_seq(seq);
    cm.set_tick(tick);
    cm.set_timestamp(ts);
    cm.set_payload(std::string(payload_size, 'P'));
    return cm;
}

/// 通过写入+读取填充，推进 ring 的 write_pos/read_pos
void AdvanceRing(void *shm, uint64_t advance)
{
    uint64_t done = 0;
    while (done < advance)
    {
        auto chunk = static_cast<uint32_t>(
            std::min<uint64_t>(advance - done, SmallRing::max_write_size));
        std::vector<char> tmp(chunk, 'X');
        ASSERT(SmallRing::TryWrite(shm, tmp.data(), chunk) == 0);
        ASSERT(SmallRing::ReadExact(shm, tmp.data(), chunk) == 0);
        done += chunk;
    }
}

// =========================================================================
// 测试 1：单帧跨环尾，遍历多个 tail 偏移（单进程，纯内存）
// =========================================================================

void TestSingleFrameWrap()
{
    std::printf("--- Test 1: single proto frame wrap-around ---\n");

    auto hb = MakeHeartbeat(42, 100, 1234567890);

    std::string type_name = hb.GetTypeName();
    auto pb_size = static_cast<uint32_t>(hb.ByteSizeLong());
    auto name_len = static_cast<uint16_t>(type_name.size());
    uint32_t payload_len = shm::kTypeNameLenSize + name_len + pb_size;
    uint32_t frame_size = shm::kMsgHeaderSize + payload_len;

    std::printf("  frame_size=%u (hdr=%u, name_prefix=%u, name=%u, pb=%u)\n",
                frame_size, shm::kMsgHeaderSize, shm::kTypeNameLenSize,
                static_cast<uint32_t>(name_len), pb_size);

    std::vector<uint8_t> pb_buf(pb_size);
    hb.SerializeToArray(pb_buf.data(), static_cast<int>(pb_size));

    uint32_t offsets[] = {
        1,
        4,
        shm::kMsgHeaderSize - 1,
        shm::kMsgHeaderSize,
        shm::kMsgHeaderSize + 1,
        shm::kMsgHeaderSize + shm::kTypeNameLenSize,
        shm::kMsgHeaderSize + shm::kTypeNameLenSize + name_len / 2,
        shm::kMsgHeaderSize + shm::kTypeNameLenSize + name_len,
        frame_size / 2,
        frame_size - 1,
    };

    int pass_count = 0;
    for (uint32_t tail : offsets)
    {
        if (tail == 0 || tail >= frame_size)
            continue;

        void *shm = std::aligned_alloc(64, SmallRing::shm_size);
        ASSERT(shm != nullptr);
        SmallRing::Init(shm);
        AdvanceRing(shm, kSmallCap - tail);

        // 用 BatchWriter 手动写一帧
        SmallRing::BatchWriter batch(shm);

        shm::MsgHeader mhdr{};
        mhdr.len = payload_len;
        mhdr.seq = 7;
        ASSERT(batch.TryWrite(&mhdr, shm::kMsgHeaderSize) == 0);
        ASSERT(batch.TryWrite(&name_len, shm::kTypeNameLenSize) == 0);
        ASSERT(batch.TryWrite(type_name.data(), name_len) == 0);
        ASSERT(batch.TryWrite(pb_buf.data(), pb_size) == 0);
        batch.Flush();

        // Peek 验证确实跨环尾
        const void *seg1 = nullptr, *seg2 = nullptr;
        uint32_t seg1_len = 0, seg2_len = 0;
        ASSERT(SmallRing::Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len) == 0);
        ASSERT(seg1_len + seg2_len == frame_size);
        ASSERT(seg2_len > 0);

        // 拼接到连续缓冲区
        std::vector<char> frame_buf(frame_size);
        std::memcpy(frame_buf.data(), seg1, seg1_len);
        std::memcpy(frame_buf.data() + seg1_len, seg2, seg2_len);

        // DecodeHeader + DecodeFrom
        const char *dec_type_name = nullptr;
        uint32_t dec_type_name_len = 0;
        const void *dec_payload = nullptr;
        uint32_t dec_payload_len = 0;
        uint32_t dec_seq = 0;
        ASSERT(shm::ProtoCodec<shm_ipc::HeartbeatProto>::DecodeHeader(
            frame_buf.data(), frame_size,
            &dec_type_name, &dec_type_name_len,
            &dec_payload, &dec_payload_len, &dec_seq));
        ASSERT(dec_seq == 7);
        ASSERT(std::string(dec_type_name, dec_type_name_len) == type_name);

        shm_ipc::HeartbeatProto out;
        ASSERT(shm::ProtoCodec<shm_ipc::HeartbeatProto>::DecodeFrom(
            dec_payload, dec_payload_len, &out));
        ASSERT(out.client_id() == 42);
        ASSERT(out.seq() == 100);
        ASSERT(out.timestamp() == 1234567890);

        std::free(shm);
        ++pass_count;
    }

    std::printf("  PASS: %d tail offsets\n", pass_count);
}

// =========================================================================
// 测试 2：跨进程 ProtoCodec::Send + FrameReader 经小容量 ring 跨环尾
// =========================================================================

void TestSendRecvWrap()
{
    std::printf("--- Test 2: ProtoCodec Send + FrameReader cross-process wrap ---\n");

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        std::perror("socketpair");
        std::abort();
    }

    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); std::abort(); }

    if (pid == 0)
    {
        ::close(sv[0]);
        auto ch = SmallChannel::Connect(sv[1]);

        // 发送 3 帧填充（每帧 payload 1024B → 帧 1032B），推进 ~3096
        char filler[1024];
        std::memset(filler, 'F', sizeof(filler));
        for (int i = 0; i < 3; ++i)
            while (shm::Send(ch, filler, sizeof(filler),
                             static_cast<uint32_t>(i)) != 0)
                ;

        // 等 parent 消费
        char ack = 0;
        (void)::read(sv[1], &ack, 1);

        // 发送 protobuf 帧（会跨环尾）
        auto hb = MakeHeartbeat(99, 200, 9876543210LL);
        int rc = shm::ProtoCodec<shm_ipc::HeartbeatProto>::Send(ch, hb, 50);
        if (rc != 0) { std::fprintf(stderr, "child: Send hb failed\n"); std::_Exit(1); }

        auto cm = MakeClientMsg(3, 4, 55555, 128);
        rc = shm::ProtoCodec<shm_ipc::ClientMsgProto>::Send(ch, cm, 51);
        if (rc != 0) { std::fprintf(stderr, "child: Send cm failed\n"); std::_Exit(1); }

        (void)::read(sv[1], &ack, 1);
        ::close(sv[1]);
        ::_exit(0);
    }

    ::close(sv[1]);
    auto ch = SmallChannel::Accept(sv[0]);

    shm::FrameReader<> reader;

    // 消费 3 帧填充
    for (int i = 0; i < 3; ++i)
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        while (reader.TryRecv(ch, &payload, &payload_len) != 0)
            ;
    }
    reader.Commit(ch);

    char ack = 1;
    (void)::write(sv[0], &ack, 1);

    // 读取 HeartbeatProto
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        while (reader.TryRecv(ch, &payload, &payload_len) != 0)
            ;

        const void *pb_data = nullptr;
        uint32_t pb_len = 0;
        shm::ProtoCodec<shm_ipc::HeartbeatProto> codec;
        ASSERT(codec.DecodePayload(payload, payload_len, &pb_data, &pb_len));

        shm_ipc::HeartbeatProto hb_out;
        ASSERT(shm::ProtoCodec<shm_ipc::HeartbeatProto>::DecodeFrom(
            pb_data, pb_len, &hb_out));
        ASSERT(hb_out.client_id() == 99);
        ASSERT(hb_out.seq() == 200);
        ASSERT(hb_out.timestamp() == 9876543210LL);
        ASSERT(reader.LastSeq() == 50);
        std::printf("  HeartbeatProto: OK (client_id=%d, seq=%d)\n",
                    hb_out.client_id(), hb_out.seq());
    }

    // 读取 ClientMsgProto
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        while (reader.TryRecv(ch, &payload, &payload_len) != 0)
            ;

        const void *pb_data = nullptr;
        uint32_t pb_len = 0;
        shm::ProtoCodec<shm_ipc::ClientMsgProto> codec;
        ASSERT(codec.DecodePayload(payload, payload_len, &pb_data, &pb_len));

        shm_ipc::ClientMsgProto cm_out;
        ASSERT(shm::ProtoCodec<shm_ipc::ClientMsgProto>::DecodeFrom(
            pb_data, pb_len, &cm_out));
        ASSERT(cm_out.seq() == 3);
        ASSERT(cm_out.tick() == 4);
        ASSERT(cm_out.timestamp() == 55555);
        ASSERT(cm_out.payload().size() == 128);
        for (size_t i = 0; i < cm_out.payload().size(); ++i)
            ASSERT(cm_out.payload()[i] == 'P');
        ASSERT(reader.LastSeq() == 51);
        std::printf("  ClientMsgProto: OK (seq=%d, payload_size=%zu)\n",
                    cm_out.seq(), cm_out.payload().size());
    }

    reader.Commit(ch);

    (void)::write(sv[0], &ack, 1);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ::close(sv[0]);
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 3：连续多帧回绕，混合大小 protobuf
// =========================================================================

void TestMultiFrameContinuousWrap()
{
    std::printf("--- Test 3: multi-frame continuous wrap ---\n");

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        std::perror("socketpair");
        std::abort();
    }

    constexpr int kCount = 30;

    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); std::abort(); }

    if (pid == 0)
    {
        ::close(sv[0]);
        auto ch = SmallChannel::Connect(sv[1]);

        for (int i = 0; i < kCount; ++i)
        {
            if (i % 2 == 0)
            {
                auto hb = MakeHeartbeat(i, i * 10, i * 100);
                while (shm::ProtoCodec<shm_ipc::HeartbeatProto>::Send(
                           ch, hb, static_cast<uint32_t>(i)) != 0)
                    ;
            }
            else
            {
                auto cm = MakeClientMsg(i, i * 10, i * 100, 200);
                while (shm::ProtoCodec<shm_ipc::ClientMsgProto>::Send(
                           ch, cm, static_cast<uint32_t>(i)) != 0)
                    ;
            }
        }

        char ack = 0;
        (void)::read(sv[1], &ack, 1);
        ::close(sv[1]);
        ::_exit(0);
    }

    ::close(sv[1]);
    auto ch = SmallChannel::Accept(sv[0]);

    shm::FrameReader<> reader;

    for (int i = 0; i < kCount; ++i)
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        while (reader.TryRecv(ch, &payload, &payload_len) != 0)
            ;

        if (i % 2 == 0)
        {
            const void *pb_data = nullptr;
            uint32_t pb_len = 0;
            shm::ProtoCodec<shm_ipc::HeartbeatProto> codec;
            ASSERT(codec.DecodePayload(payload, payload_len, &pb_data, &pb_len));

            shm_ipc::HeartbeatProto hb_out;
            ASSERT(shm::ProtoCodec<shm_ipc::HeartbeatProto>::DecodeFrom(
                pb_data, pb_len, &hb_out));
            ASSERT(hb_out.client_id() == i);
            ASSERT(hb_out.seq() == i * 10);
            ASSERT(hb_out.timestamp() == i * 100);
        }
        else
        {
            const void *pb_data = nullptr;
            uint32_t pb_len = 0;
            shm::ProtoCodec<shm_ipc::ClientMsgProto> codec;
            ASSERT(codec.DecodePayload(payload, payload_len, &pb_data, &pb_len));

            shm_ipc::ClientMsgProto cm_out;
            ASSERT(shm::ProtoCodec<shm_ipc::ClientMsgProto>::DecodeFrom(
                pb_data, pb_len, &cm_out));
            ASSERT(cm_out.seq() == i);
            ASSERT(cm_out.tick() == i * 10);
            ASSERT(cm_out.timestamp() == i * 100);
            ASSERT(cm_out.payload().size() == 200);
        }
    }

    reader.Commit(ch);

    char ack = 1;
    (void)::write(sv[0], &ack, 1);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    ::close(sv[0]);
    std::printf("  PASS: %d frames (mixed HeartbeatProto + ClientMsgProto)\n",
                kCount);
}

}  // anonymous namespace

int main()
{
    std::printf("=== ProtoCodec wrap-around test suite (Capacity=%zu) ===\n\n",
                kSmallCap);

    TestSingleFrameWrap();
    std::printf("\n");
    TestSendRecvWrap();
    std::printf("\n");
    TestMultiFrameContinuousWrap();

    std::printf("\n=== TEST PASSED ===\n");
    return 0;
}
