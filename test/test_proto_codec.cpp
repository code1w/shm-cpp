/**
 * @file test_proto_codec.cpp
 * @brief ProtoCodec 编解码测试
 *
 * 测试 ProtoCodec<T> 通过 RingChannel 编码 / 解码 protobuf 消息。
 */

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include <shm_ipc/common.hpp>
#include <shm_ipc/frame_reader.hpp>
#include <shm_ipc/proto_codec.hpp>
#include <shm_ipc/ring_channel.hpp>

#include "shm_messages.pb.h"

namespace {

using Channel = shm::DefaultRingChannel;

/// 测试 1：EncodeProto / DecodeProtoHeader / DecodeProto 纯缓冲区编解码
void TestEncodeDecodeBuffer()
{
    std::printf("--- TestEncodeDecodeBuffer ---\n");

    shm_ipc::HeartbeatProto hb;
    hb.set_client_id(42);
    hb.set_seq(100);
    hb.set_timestamp(1234567890);

    char buf[1024]{};
    uint32_t frame_size = shm::ProtoCodec<shm_ipc::HeartbeatProto>::EncodeTo(hb, buf, sizeof(buf), 7);
    assert(frame_size > 0);

    // 解码帧头
    const char *type_name = nullptr;
    uint32_t type_name_len = 0;
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    uint32_t seq = 0;
    bool ok = shm::ProtoCodec<shm_ipc::HeartbeatProto>::DecodeHeader(buf, frame_size,
                                     &type_name, &type_name_len,
                                     &payload, &payload_len, &seq);
    assert(ok);
    assert(seq == 7);

    std::string tn(type_name, type_name_len);
    assert(tn == "shm_ipc.HeartbeatProto");

    // 反序列化
    shm_ipc::HeartbeatProto out;
    ok = shm::ProtoCodec<shm_ipc::HeartbeatProto>::DecodeFrom(payload, payload_len, &out);
    assert(ok);
    assert(out.client_id() == 42);
    assert(out.seq() == 100);
    assert(out.timestamp() == 1234567890);

    std::printf("  PASS\n");
}

/// 测试 2：ProtoCodec<T> ICodec 接口
void TestProtoCodecInterface()
{
    std::printf("--- TestProtoCodecInterface ---\n");

    shm::ProtoCodec<shm_ipc::HeartbeatProto> codec;

    shm_ipc::HeartbeatProto hb;
    hb.set_client_id(7);
    hb.set_seq(88);
    hb.set_timestamp(999);

    // Encode
    char buf[1024]{};
    uint32_t frame_size = codec.Encode(&hb, buf, sizeof(buf), 3);
    assert(frame_size > 0);

    // Decode
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    uint32_t seq = 0;
    bool ok = codec.Decode(buf, frame_size, &payload, &payload_len, &seq);
    assert(ok);
    assert(seq == 3);

    // DecodePayload（跳过 type_name 前缀）
    const void *data = nullptr;
    uint32_t data_len = 0;
    ok = codec.DecodePayload(
        static_cast<const char *>(static_cast<const void *>(buf)) + shm::kMsgHeaderSize,
        frame_size - shm::kMsgHeaderSize,
        &data, &data_len);
    assert(ok);

    shm_ipc::HeartbeatProto out;
    ok = out.ParseFromArray(data, static_cast<int>(data_len));
    assert(ok);
    assert(out.client_id() == 7);
    assert(out.seq() == 88);

    // TypeName
    assert(codec.TypeName() == "shm_ipc.HeartbeatProto");

    std::printf("  PASS\n");
}

/// 测试 3：SendProto 通过 RingChannel 发送 + FrameReader 接收
void TestSendRecvViaRing()
{
    std::printf("--- TestSendRecvViaRing ---\n");

    // 创建 socketpair 用于握手
    int sv[2];
    int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    // 在子线程/fork 中不好用 socketpair+Accept/Connect，
    // 这里用更底层的方式：直接创建 memfd + ring
    // 但 RingChannel::Accept/Connect 需要 fd 交换，所以用 fork

    pid_t pid = ::fork();
    assert(pid >= 0);

    if (pid == 0)
    {
        // 子进程：writer
        ::close(sv[0]);
        auto ch = Channel::Connect(sv[1]);

        shm_ipc::HeartbeatProto hb;
        hb.set_client_id(1);
        hb.set_seq(42);
        hb.set_timestamp(12345);
        rc = shm::ProtoCodec<shm_ipc::HeartbeatProto>::Send(ch, hb, 10);
        assert(rc == 0);

        shm_ipc::ClientMsgProto cm;
        cm.set_seq(2);
        cm.set_tick(3);
        cm.set_timestamp(99999);
        cm.set_payload(std::string(128, 'X'));
        rc = shm::ProtoCodec<shm_ipc::ClientMsgProto>::Send(ch, cm, 11);
        assert(rc == 0);

        // 等 parent 读完
        char ack = 0;
        (void)::read(sv[1], &ack, 1);
        ::close(sv[1]);
        ::_exit(0);
    }

    // 父进程：reader
    ::close(sv[1]);
    auto ch = Channel::Accept(sv[0]);

    shm::FrameReader<> reader;

    // 读第一条消息
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    while (reader.TryRecv(ch, &payload, &payload_len) != 0)
        ;

    // payload 是 [type_name_len][type_name][pb_data]
    assert(payload_len >= shm::kTypeNameLenSize);
    uint16_t name_len = 0;
    std::memcpy(&name_len, payload, sizeof(name_len));
    std::string type_name(static_cast<const char *>(payload) + shm::kTypeNameLenSize, name_len);
    assert(type_name == "shm_ipc.HeartbeatProto");

    const void *pb_start = static_cast<const char *>(payload) + shm::kTypeNameLenSize + name_len;
    uint32_t pb_size = payload_len - shm::kTypeNameLenSize - name_len;

    shm_ipc::HeartbeatProto hb_out;
    bool ok = hb_out.ParseFromArray(pb_start, static_cast<int>(pb_size));
    assert(ok);
    assert(hb_out.client_id() == 1);
    assert(hb_out.seq() == 42);
    assert(hb_out.timestamp() == 12345);

    // 读第二条消息
    while (reader.TryRecv(ch, &payload, &payload_len) != 0)
        ;
    std::memcpy(&name_len, payload, sizeof(name_len));
    type_name.assign(static_cast<const char *>(payload) + shm::kTypeNameLenSize, name_len);
    assert(type_name == "shm_ipc.ClientMsgProto");

    pb_start = static_cast<const char *>(payload) + shm::kTypeNameLenSize + name_len;
    pb_size = payload_len - shm::kTypeNameLenSize - name_len;

    shm_ipc::ClientMsgProto cm_out;
    ok = cm_out.ParseFromArray(pb_start, static_cast<int>(pb_size));
    assert(ok);
    assert(cm_out.seq() == 2);
    assert(cm_out.tick() == 3);
    assert(cm_out.payload().size() == 128);

    reader.Commit(ch);

    // 通知子进程退出
    char ack = 1;
    (void)::write(sv[0], &ack, 1);
    int status = 0;
    ::waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    ::close(sv[0]);
    std::printf("  PASS\n");
}

}  // anonymous namespace

int main()
{
    TestEncodeDecodeBuffer();
    TestProtoCodecInterface();
    TestSendRecvViaRing();

    std::printf("\n=== TEST PASSED ===\n");
    return 0;
}
