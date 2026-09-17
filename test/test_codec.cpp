/**
 * @file test_codec.cpp
 * @brief Codec 编解码测试
 *
 * 测试内容：
 * 1. 通用 Encode/Decode 往返
 * 2. EncodePod/DecodePod 便利层
 * 3. SendPod + FrameReader 跨进程完整流程
 * 4. 逐字节写入字节流，FrameReader 在所有字节到达后解码
 * 5. 变长消息 Send + FrameReader 跨进程
 * 6. 类型标签校验 + Decode 长度溢出防护（回归）
 * 7. 超大 payload Send 拒绝且不损坏字节流（回归）
 * 8. FrameReader 大帧零拷贝不受回退缓冲区限制（回归）
 * 9. 跨环尾超大帧返回 -2 后自动丢弃自愈（回归）
 * 10. SendFrameBatch 回滚同时恢复 Count（回归）
 * 11. 非法帧头长度返回 -3 协议错误而非永久等待（回归）
 * 12. PodCodec 毒帧立即丢弃并自动跳过（回归）
 * 13. 单帧在任意字节边界切成两段到达，接收方正确拆包（穷尽切分）
 * 14. 多帧字节流被随机大小块切分到达，接收方按序还原全部帧
 *
 * 使用 fork + socketpair，与 bench_shm.cpp 相同的进程隔离方式。
 */

#include <shm_ipc/codec.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/pod_codec.hpp>
#include <shm_ipc/ring_channel.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Channel = shm::DefaultRingChannel;

constexpr uint32_t kMagicPayloadLen = 2048;
constexpr char     kFillByte        = 'X';
constexpr int32_t  kSeq             = 42;
constexpr int32_t  kTick            = 7;
constexpr int64_t  kTimestamp       = 1234567890;
constexpr uint32_t kMsgSeq         = 99;

/// 构造一个已知内容的 ClientMsg
ClientMsg MakeTestMsg()
{
    ClientMsg msg{};
    msg.seq         = kSeq;
    msg.tick        = kTick;
    msg.timestamp   = kTimestamp;
    msg.payload_len = kMagicPayloadLen;
    std::memset(msg.payload, kFillByte, kMagicPayloadLen);
    return msg;
}

/// 校验解码出的 ClientMsg 内容完全一致
void VerifyMsg(const ClientMsg& msg)
{
    if (msg.seq != kSeq) { std::fprintf(stderr, "FAIL: seq\n"); std::abort(); }
    if (msg.tick != kTick) { std::fprintf(stderr, "FAIL: tick\n"); std::abort(); }
    if (msg.timestamp != kTimestamp) { std::fprintf(stderr, "FAIL: timestamp\n"); std::abort(); }
    if (msg.payload_len != kMagicPayloadLen) { std::fprintf(stderr, "FAIL: payload_len\n"); std::abort(); }
    for (uint32_t i = 0; i < kMagicPayloadLen; ++i)
    {
        if (msg.payload[i] != kFillByte) { std::fprintf(stderr, "FAIL: payload[%u]\n", i); std::abort(); }
    }
    std::printf("  PASS: all fields match\n");
}

// =========================================================================
// 测试 1：通用 Encode/Decode 往返
// =========================================================================

void TestEncodeDecode()
{
    std::printf("--- Test 1: Encode/Decode round-trip ---\n");

    const char data[] = "hello codec";
    uint32_t data_len = static_cast<uint32_t>(std::strlen(data));
    constexpr uint32_t seq = 777;

    char buf[256];
    uint32_t frame_size = shm::Encode(data, data_len, buf, sizeof(buf), seq);
    if (frame_size == 0 ||
        frame_size != shm::kMsgHeaderSize + data_len)
    {
        std::fprintf(stderr, "FAIL: Encode\n");
        std::abort();
    }

    const void *out_payload = nullptr;
    uint32_t out_len = 0;
    uint32_t out_seq = 0;
    if (!shm::Decode(buf, frame_size, &out_payload, &out_len, &out_seq))
    {
        std::fprintf(stderr, "FAIL: Decode\n");
        std::abort();
    }
    if (out_len != data_len || out_seq != seq ||
        std::memcmp(out_payload, data, data_len) != 0)
    {
        std::fprintf(stderr, "FAIL: field mismatch\n");
        std::abort();
    }

    // 缓冲区太小应返回 0
    if (shm::Encode(data, data_len, buf, 4, seq) != 0)
    {
        std::fprintf(stderr, "FAIL: small buf should return 0\n");
        std::abort();
    }

    std::printf("  PASS: Encode/Decode round-trip correct\n");
}

// =========================================================================
// 测试 2：EncodePod/DecodePod 往返
// =========================================================================

void TestEncodePodDecodePod()
{
    std::printf("--- Test 2: EncodePod/DecodePod round-trip ---\n");

    Heartbeat hb{};
    hb.client_id = 10;
    hb.seq       = 77;
    hb.timestamp = 555;

    constexpr uint32_t frame_size = shm::kMsgHeaderSize + shm::kTagSize + sizeof(Heartbeat);
    char buf[frame_size];

    uint32_t send_seq = 12345;
    if (shm::PodCodec<Heartbeat>::EncodeTo(hb, buf, frame_size, send_seq) != frame_size)
    {
        std::fprintf(stderr, "FAIL: EncodePod\n");
        std::abort();
    }

    // Decode 帧头
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    uint32_t recv_seq = 0;
    if (!shm::Decode(buf, frame_size, &payload, &payload_len, &recv_seq))
    {
        std::fprintf(stderr, "FAIL: Decode\n");
        std::abort();
    }

    // payload = [tag u32][Heartbeat bytes], 跳过 tag
    if (payload_len < shm::kTagSize)
    {
        std::fprintf(stderr, "FAIL: payload too small\n");
        std::abort();
    }
    const void *pod_data = static_cast<const char *>(payload) + shm::kTagSize;
    uint32_t pod_len = payload_len - shm::kTagSize;

    // DecodePod
    Heartbeat out{};
    if (!shm::PodCodec<Heartbeat>::DecodeFrom(pod_data, pod_len, &out))
    {
        std::fprintf(stderr, "FAIL: DecodePod\n");
        std::abort();
    }

    if (recv_seq != send_seq || out.client_id != 10 || out.seq != 77 || out.timestamp != 555)
    {
        std::fprintf(stderr, "FAIL: field mismatch\n");
        std::abort();
    }

    std::printf("  PASS: EncodePod/DecodePod round-trip correct (seq=%u)\n", recv_seq);
}

// =========================================================================
// 测试 3：SendPod + FrameReader 跨进程
// =========================================================================

void RunSenderBasic(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    ClientMsg msg = MakeTestMsg();
    if (shm::PodCodec<ClientMsg>::Send(ch, msg, kMsgSeq) != 0)
    {
        std::fprintf(stderr, "FAIL: Send\n");
        std::abort();
    }

    std::printf("sender(basic): sent 1 ClientMsg via Send\n");

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverBasic(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    shm::FrameReader<> reader;

    std::printf("receiver(basic): polling for frame...\n");

    pollfd pfd{};
    pfd.fd     = efd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout\n");
        std::abort();
    }
    Channel::DrainNotify(efd);

    uint32_t tag = 0;
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    if (reader.TryRecv(ch, &payload, &payload_len) != 0)
    {
        std::fprintf(stderr, "FAIL: TryRecv\n");
        std::abort();
    }

    // payload = [tag u32][ClientMsg bytes]
    std::memcpy(&tag, payload, shm::kTagSize);
    const void *pod_data = static_cast<const char *>(payload) + shm::kTagSize;
    uint32_t pod_len = payload_len - shm::kTagSize;

    ClientMsg out{};
    if (!shm::PodCodec<ClientMsg>::DecodeFrom(pod_data, pod_len, &out))
    {
        std::fprintf(stderr, "FAIL: DecodePod\n");
        std::abort();
    }

    std::printf("receiver(basic): decoded successfully\n");
    VerifyMsg(out);

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 4：逐字节写入字节流，FrameReader 在所有字节到达后成功解码
// =========================================================================

void RunSenderByteByByte(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    ClientMsg msg = MakeTestMsg();

    constexpr uint32_t frame_size = shm::kMsgHeaderSize + shm::kTagSize + sizeof(ClientMsg);
    char frame[frame_size];
    if (shm::PodCodec<ClientMsg>::EncodeTo(msg, frame, frame_size, kMsgSeq) != frame_size)
    {
        std::fprintf(stderr, "FAIL: EncodePod\n");
        std::abort();
    }

    std::printf("sender(byte-by-byte): frame_size=%u, sending byte-by-byte\n", frame_size);

    for (uint32_t i = 0; i < frame_size; ++i)
    {
        if (ch.TryWrite(&frame[i], 1) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite byte %u\n", i);
            std::abort();
        }
        ch.NotifyPeer();
    }

    std::printf("sender(byte-by-byte): all %u bytes sent\n", frame_size);

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverByteByByte(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    shm::FrameReader<> reader;

    int wakeups  = 0;
    bool decoded = false;

    constexpr uint32_t frame_size = shm::kMsgHeaderSize + shm::kTagSize + sizeof(ClientMsg);
    std::printf("receiver(byte-by-byte): expecting frame_size=%u, polling...\n", frame_size);

    const void *payload = nullptr;
    uint32_t payload_len = 0;

    while (!decoded)
    {
        pollfd pfd{};
        pfd.fd     = efd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 5000) <= 0)
        {
            std::fprintf(stderr, "receiver: poll timeout/error, wakeups=%d\n", wakeups);
            std::abort();
        }

        Channel::DrainNotify(efd);
        ++wakeups;

        if (reader.TryRecv(ch, &payload, &payload_len) == 0)
            decoded = true;
    }

    // payload = [tag u32][ClientMsg bytes]
    const void *pod_data = static_cast<const char *>(payload) + shm::kTagSize;
    uint32_t pod_len = payload_len - shm::kTagSize;

    ClientMsg out{};
    if (!shm::PodCodec<ClientMsg>::DecodeFrom(pod_data, pod_len, &out))
    {
        std::fprintf(stderr, "FAIL: DecodePod\n");
        std::abort();
    }

    std::printf("receiver(byte-by-byte): decoded after %d wakeups\n", wakeups);
    VerifyMsg(out);

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 5：变长消息 Send + FrameReader 跨进程
// =========================================================================

constexpr uint32_t kVarSeq = 555;

void RunSenderVar(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    const char *msgs[] = {"short", "medium length message for testing",
                          "a]longer]payload]with]various]characters]0123456789"};
    for (int i = 0; i < 3; ++i)
    {
        uint32_t len = static_cast<uint32_t>(std::strlen(msgs[i]));
        if (shm::Send(ch, msgs[i], len, kVarSeq + i) != 0)
        {
            std::fprintf(stderr, "FAIL: Send[%d]\n", i);
            std::abort();
        }
    }

    std::printf("sender(var): sent 3 variable-length messages\n");

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverVar(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    shm::FrameReader<> reader;

    const char *expected[] = {"short", "medium length message for testing",
                              "a]longer]payload]with]various]characters]0123456789"};

    for (int i = 0; i < 3; ++i)
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        int rc = reader.TryRecv(ch, &payload, &payload_len);

        while (rc != 0)
        {
            pollfd pfd{};
            pfd.fd     = efd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 5000) <= 0)
            {
                std::fprintf(stderr, "FAIL: poll timeout on msg %d\n", i);
                std::abort();
            }
            Channel::DrainNotify(efd);
            rc = reader.TryRecv(ch, &payload, &payload_len);
        }

        uint32_t expected_len = static_cast<uint32_t>(std::strlen(expected[i]));
        if (payload_len != expected_len ||
            std::memcmp(payload, expected[i], expected_len) != 0)
        {
            std::fprintf(stderr, "FAIL: msg[%d] content mismatch (len=%u)\n",
                         i, payload_len);
            std::abort();
        }
        if (reader.LastSeq() != kVarSeq + static_cast<uint32_t>(i))
        {
            std::fprintf(stderr, "FAIL: msg[%d] seq mismatch\n", i);
            std::abort();
        }
    }

    std::printf("receiver(var): all 3 variable-length messages decoded correctly\n");
    std::printf("  PASS: Send/FrameReader round-trip correct\n");

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 6（回归）：类型标签校验 + Decode 长度溢出防护
// =========================================================================

void TestTagValidation()
{
    std::printf("--- Test 6: type tag validation + Decode overflow guard ---\n");

    // 编码一条 ClientMsg 帧
    ClientMsg msg = MakeTestMsg();
    constexpr uint32_t kFrame = shm::kMsgHeaderSize + shm::kTagSize + sizeof(ClientMsg);
    char buf[kFrame];
    if (shm::PodCodec<ClientMsg>::EncodeTo(msg, buf, kFrame, kMsgSeq) != kFrame)
    {
        std::fprintf(stderr, "FAIL: EncodeTo\n");
        std::abort();
    }

    const void *payload = nullptr;
    uint32_t payload_len = 0;
    if (!shm::Decode(buf, kFrame, &payload, &payload_len, nullptr))
    {
        std::fprintf(stderr, "FAIL: Decode\n");
        std::abort();
    }

    shm::PodCodec<ClientMsg> good_codec;
    shm::PodCodec<Heartbeat> bad_codec;
    const void *data = nullptr;
    uint32_t data_len = 0;

    // 类型匹配：必须通过
    if (!good_codec.DecodePayload(payload, payload_len, &data, &data_len) ||
        data_len != sizeof(ClientMsg))
    {
        std::fprintf(stderr, "FAIL: matching tag wrongly rejected\n");
        std::abort();
    }

    // 类型不匹配（Heartbeat 解析 ClientMsg 帧）：必须拒绝
    if (bad_codec.DecodePayload(payload, payload_len, &data, &data_len))
    {
        std::fprintf(stderr, "FAIL: tag mismatch not detected (DecodePayload)\n");
        std::abort();
    }
    if (bad_codec.Decode(buf, kFrame, &data, &data_len, nullptr))
    {
        std::fprintf(stderr, "FAIL: tag mismatch not detected (Decode)\n");
        std::abort();
    }
    if (!good_codec.Decode(buf, kFrame, &data, &data_len, nullptr))
    {
        std::fprintf(stderr, "FAIL: matching frame wrongly rejected (Decode)\n");
        std::abort();
    }

    // 长度溢出：hdr.len = 0xFFFFFFFF 时 kMsgHeaderSize + len 回绕，
    // 必须被拒绝而不是返回巨大 payload_len
    char evil[shm::kMsgHeaderSize];
    shm::MsgHeader hdr{};
    hdr.len = 0xFFFFFFFFu;
    hdr.seq = 0;
    std::memcpy(evil, &hdr, sizeof(hdr));
    if (shm::Decode(evil, sizeof(evil), &payload, &payload_len, nullptr))
    {
        std::fprintf(stderr, "FAIL: overflow length not rejected\n");
        std::abort();
    }

    std::printf("  PASS: tag validated, overflow rejected\n");
}

// =========================================================================
// 测试 7（回归）：超大 payload Send 拒绝且不损坏字节流
// =========================================================================

void RunSenderOversized(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    // payload 超过 max_write_size（Capacity/2）：必须返回 -1 且不写入任何数据
    std::vector<char> big(Channel::max_write_size + 1, 'B');
    if (shm::Send(ch, big.data(), static_cast<uint32_t>(big.size()), 1) == 0)
    {
        std::fprintf(stderr, "FAIL: oversized Send should be rejected\n");
        std::abort();
    }

    // 紧接着发送一条合法消息，检验字节流未被破坏
    ClientMsg msg = MakeTestMsg();
    if (shm::PodCodec<ClientMsg>::Send(ch, msg, kMsgSeq) != 0)
    {
        std::fprintf(stderr, "FAIL: Send after oversized rejection\n");
        std::abort();
    }

    std::printf("sender(oversized): oversized rejected, valid msg sent\n");

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverOversized(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    pollfd pfd{};
    pfd.fd     = efd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout\n");
        std::abort();
    }
    Channel::DrainNotify(efd);

    shm::PodCodec<ClientMsg> codec;
    ClientMsg out{};
    if (codec.Recv(ch, &out) != 0)
    {
        std::fprintf(stderr, "FAIL: stream corrupted by rejected oversized send\n");
        std::abort();
    }
    VerifyMsg(out);

    // 不应有任何残留帧（被拒绝的帧不能有半个 header 留在环里）
    if (codec.Recv(ch, &out) == 0)
    {
        std::fprintf(stderr, "FAIL: phantom frame from rejected send\n");
        std::abort();
    }

    std::printf("  PASS: oversized rejected, stream intact\n");

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 8（回归）：FrameReader 大帧零拷贝不受回退缓冲区 BufSize 限制
// =========================================================================

constexpr uint32_t kLargePayloadLen = 65536;  // 远大于下面的 BufSize=128

void RunSenderLargeFrame(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    std::vector<char> body(kLargePayloadLen, 'L');
    if (shm::Send(ch, body.data(), kLargePayloadLen, 7) != 0)
    {
        std::fprintf(stderr, "FAIL: Send large frame\n");
        std::abort();
    }

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverLargeFrame(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    pollfd pfd{};
    pfd.fd     = efd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout\n");
        std::abort();
    }
    Channel::DrainNotify(efd);

    // 回退缓冲区仅 128B，但帧连续存放在环内，零拷贝快速路径不应受限
    shm::FrameReader<128> reader;
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    if (reader.TryRecv(ch, &payload, &payload_len) != 0)
    {
        std::fprintf(stderr, "FAIL: zero-copy fast path wrongly limited by BufSize\n");
        std::abort();
    }
    if (payload_len != kLargePayloadLen)
    {
        std::fprintf(stderr, "FAIL: large frame len=%u\n", payload_len);
        std::abort();
    }
    auto *p = static_cast<const char *>(payload);
    for (uint32_t i = 0; i < kLargePayloadLen; ++i)
    {
        if (p[i] != 'L')
        {
            std::fprintf(stderr, "FAIL: large frame content[%u]\n", i);
            std::abort();
        }
    }

    std::printf("  PASS: %u-byte frame zero-copied with BufSize=128\n",
                kLargePayloadLen);

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 9（回归）：跨环尾超大帧返回 -2，下一帧自动丢弃毒帧并恢复正常
// =========================================================================

using SmallChannel = shm::RingChannel<4096>;

constexpr uint32_t kFillerLen = 2032;  // 两帧填充到环尾（2×2040 = 4080）
constexpr uint32_t kPoisonLen = 512;   // 毒帧 payload，跨环尾且 > BufSize(128)

void RunSenderPoison(int socket_fd)
{
    auto ch = SmallChannel::Accept(socket_fd);

    // 阶段 1：两条填充帧把写位置推到环尾附近
    std::vector<char> filler(kFillerLen, 'F');
    if (shm::Send(ch, filler.data(), kFillerLen, 1) != 0 ||
        shm::Send(ch, filler.data(), kFillerLen, 2) != 0)
    {
        std::fprintf(stderr, "FAIL: Send filler\n");
        std::abort();
    }

    char ack = 0;
    ::read(socket_fd, &ack, 1);  // 等接收方消费掉填充帧

    // 阶段 2：毒帧（payload 512B，跨环尾）+ 一条后续正常帧
    std::vector<char> poison(kPoisonLen, 'P');
    if (shm::Send(ch, poison.data(), kPoisonLen, 3) != 0)
    {
        std::fprintf(stderr, "FAIL: Send poison\n");
        std::abort();
    }
    const char after[] = "after";
    if (shm::Send(ch, after, 5, 4) != 0)
    {
        std::fprintf(stderr, "FAIL: Send after\n");
        std::abort();
    }

    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverPoison(int socket_fd)
{
    auto ch = SmallChannel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();
    shm::FrameReader<128> reader;

    pollfd pfd{};
    pfd.fd     = efd;
    pfd.events = POLLIN;

    const void *payload = nullptr;
    uint32_t payload_len = 0;

    // 阶段 1：读取两条填充帧（将 read_pos 推进到环尾附近）
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout phase 1\n");
        std::abort();
    }
    Channel::DrainNotify(efd);
    if (reader.TryRecv(ch, &payload, &payload_len) != 0 || payload_len != kFillerLen)
    {
        std::fprintf(stderr, "FAIL: filler frame 1\n");
        std::abort();
    }
    if (reader.TryRecv(ch, &payload, &payload_len) != 0 || payload_len != kFillerLen)
    {
        std::fprintf(stderr, "FAIL: filler frame 2\n");
        std::abort();
    }
    char ack = 1;
    ::write(socket_fd, &ack, 1);

    // 阶段 2：毒帧跨环尾且超过 BufSize，应返回 -2
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout phase 2\n");
        std::abort();
    }
    Channel::DrainNotify(efd);
    int rc = reader.TryRecv(ch, &payload, &payload_len);
    if (rc != -2)
    {
        std::fprintf(stderr, "FAIL: expected -2 for wrapped oversized frame, got %d\n", rc);
        std::abort();
    }

    // 自愈：下一次 TryRecv 自动丢弃毒帧，读到后续正常帧
    rc = reader.TryRecv(ch, &payload, &payload_len);
    if (rc != 0)
    {
        std::fprintf(stderr, "FAIL: poisoned frame not auto-discarded (rc=%d)\n", rc);
        std::abort();
    }
    if (payload_len != 5 || std::memcmp(payload, "after", 5) != 0)
    {
        std::fprintf(stderr, "FAIL: wrong frame after poison discard\n");
        std::abort();
    }
    if (reader.LastSeq() != 4)
    {
        std::fprintf(stderr, "FAIL: seq after poison = %u\n", reader.LastSeq());
        std::abort();
    }

    std::printf("  PASS: -2 poisoned frame auto-discarded, stream recovered\n");

    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 10（回归）：SendFrameBatch 回滚必须同时恢复 PendingBytes 和 Count
// =========================================================================

void TestBatchRewindCount()
{
    std::printf("--- Test 10: SendFrameBatch rewind restores count ---\n");

    // max_write_size = 2048
    void *shm = ::mmap(nullptr, SmallChannel::Ring::shm_size,
                       PROT_READ | PROT_WRITE,
                       MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (shm == MAP_FAILED)
    {
        std::perror("mmap");
        std::abort();
    }
    SmallChannel::Ring::Init(shm);

    // 直接构造通道级批量写入器（channel 指针仅用于 Flush 时通知，传 nullptr）
    SmallChannel::ChannelBatchWriter batch(
        SmallChannel::Ring::BatchWriter(shm), nullptr);

    char data[2100]{};

    // 第 1 帧：合法帧（header + payload，两次 TryWrite 成功）
    if (shm::SendFrameBatch<4096>(batch, 100, 1, [&](auto &b) {
            b.TryWrite(data, 100);
        }) != 0)
    {
        std::fprintf(stderr, "FAIL: frame1 SendFrameBatch\n");
        std::abort();
    }
    int count_after_frame1 = batch.Count();
    if (count_after_frame1 != 2)  // header + payload
    {
        std::fprintf(stderr, "FAIL: count after frame1 = %d\n",
                     count_after_frame1);
        std::abort();
    }

    // 第 2 帧：callback 第一次写成功、第二次写超长失败 → 触发回滚
    int rc = shm::SendFrameBatch<4096>(batch, 4 + 2100, 2, [&](auto &b) {
        b.TryWrite(data, 4);     // 成功
        b.TryWrite(data, 2100);  // 失败：> max_write_size(2048)
    });
    if (rc != -1)
    {
        std::fprintf(stderr, "FAIL: frame2 should be rolled back, rc=%d\n", rc);
        std::abort();
    }
    if (batch.PendingBytes() != shm::kMsgHeaderSize + 100)
    {
        std::fprintf(stderr, "FAIL: PendingBytes after rewind = %llu\n",
                     static_cast<unsigned long long>(batch.PendingBytes()));
        std::abort();
    }
    // 回滚必须恢复 Count：旧实现只减 1，会残留为 3
    if (batch.Count() != count_after_frame1)
    {
        std::fprintf(stderr, "FAIL: Count after rewind = %d, expect %d\n",
                     batch.Count(), count_after_frame1);
        std::abort();
    }
    if (batch.Flush() != count_after_frame1)
    {
        std::fprintf(stderr, "FAIL: Flush count\n");
        std::abort();
    }

    std::printf("  PASS: rewind restores PendingBytes and Count\n");
    ::munmap(shm, SmallChannel::Ring::shm_size);
}

// =========================================================================
// 测试 11（回归）：非法帧头长度返回 -3 协议错误而非永久等待
// =========================================================================

void RunSenderBadLen(int socket_fd)
{
    auto ch = SmallChannel::Accept(socket_fd);

    // 帧头声称 payload 为 0x7FFFFFFF，远超环容量，永远不可能完整到达
    shm::MsgHeader hdr{};
    hdr.len = 0x7FFFFFFFu;
    hdr.seq = 1;
    if (ch.TryWrite(&hdr, sizeof(hdr)) != 0)
    {
        std::fprintf(stderr, "FAIL: TryWrite bad header\n");
        std::abort();
    }
    ch.NotifyPeer();

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverBadLen(int socket_fd)
{
    auto ch = SmallChannel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    pollfd pfd{};
    pfd.fd     = efd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout\n");
        std::abort();
    }
    Channel::DrainNotify(efd);

    shm::FrameReader<> reader;
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    int rc = reader.TryRecv(ch, &payload, &payload_len);
    if (rc != -3)
    {
        std::fprintf(stderr, "FAIL: expected -3 for impossible frame len, got %d\n",
                     rc);
        std::abort();
    }

    std::printf("  PASS: impossible frame length rejected with -3\n");

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 12（回归）：PodCodec 毒帧立即丢弃并自动跳过，同批好帧正常送达
// =========================================================================

void RunSenderMixedTypes(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    // 先写一条 Heartbeat 帧（对接收方 PodCodec<ClientMsg> 而言 tag 不匹配），
    // 再写一条正常 ClientMsg 帧，同一批 Flush + 一次通知
    Heartbeat hb{};
    hb.client_id = 1;
    hb.seq       = 1;
    hb.timestamp = 1;
    if (shm::PodCodec<Heartbeat>::Send(ch, hb, 1) != 0)
    {
        std::fprintf(stderr, "FAIL: Send Heartbeat\n");
        std::abort();
    }
    ClientMsg msg = MakeTestMsg();
    if (shm::PodCodec<ClientMsg>::Send(ch, msg, kMsgSeq) != 0)
    {
        std::fprintf(stderr, "FAIL: Send ClientMsg\n");
        std::abort();
    }

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverMixedTypes(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    pollfd pfd{};
    pfd.fd     = efd;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 5000) <= 0)
    {
        std::fprintf(stderr, "FAIL: poll timeout\n");
        std::abort();
    }
    Channel::DrainNotify(efd);

    shm::PodCodec<ClientMsg> codec;
    ClientMsg out{};

    // 毒帧（Heartbeat tag）应被立即丢弃并自动跳过，
    // 一次 Recv 直接拿到后面的 ClientMsg，无需等待新的通知
    if (codec.Recv(ch, &out) != 0)
    {
        std::fprintf(stderr, "FAIL: poison frame not skipped\n");
        std::abort();
    }
    VerifyMsg(out);

    // 两帧都已消费，环应为空
    if (codec.Recv(ch, &out) == 0 || ch.Readable() != 0)
    {
        std::fprintf(stderr, "FAIL: ring not fully drained\n");
        std::abort();
    }

    std::printf("  PASS: poison frame discarded eagerly, good frame delivered\n");

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 13（穷尽切分）：一帧在任意字节边界被切成两段到达
//
// 模拟"网络不稳定"：发送方发 100 字节，到达接收方时可能以任意
// 分组呈现。这里遍历每一个切分点 s（1..107，含帧头内部 <8 字节处），
// 验证：第一段到达时 TryRecv 返回 -1 且不消费字节；
// 第二段到达后 TryRecv 拆出完整帧且内容、seq 正确。
// =========================================================================

constexpr uint32_t kSplitPayloadLen = 100;  // 帧 = 8B header + 100B payload

/// 测试 13 的内容填充字节（确定性）
char SplitByte(uint32_t i) { return static_cast<char>(i * 7 + 3); }

void RunSenderSplit(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    // 预编码完整帧
    char payload[kSplitPayloadLen];
    for (uint32_t i = 0; i < kSplitPayloadLen; ++i)
        payload[i] = SplitByte(i);
    constexpr uint32_t kFrameSize = shm::kMsgHeaderSize + kSplitPayloadLen;
    char frame[kFrameSize];
    if (shm::Encode(payload, kSplitPayloadLen, frame, kFrameSize, kMsgSeq)
            != kFrameSize)
    {
        std::fprintf(stderr, "FAIL: Encode\n");
        std::abort();
    }

    // 每个切分点 s：先发 [0,s)，等接收方确认"读不到完整帧"，
    // 再发 [s,end)，等接收方确认"拆包正确"
    for (uint32_t s = 1; s < kFrameSize; ++s)
    {
        if (ch.TryWrite(frame, s) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite chunk1 s=%u\n", s);
            std::abort();
        }
        ch.NotifyPeer();
        char sync = 'a';
        if (::write(socket_fd, &sync, 1) != 1)
            std::abort();

        char ack = 0;
        if (::read(socket_fd, &ack, 1) != 1 || ack != '1')
        {
            std::fprintf(stderr, "FAIL: no -1 ack at split %u\n", s);
            std::abort();
        }

        if (ch.TryWrite(frame + s, kFrameSize - s) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite chunk2 s=%u\n", s);
            std::abort();
        }
        ch.NotifyPeer();
        sync = 'b';
        if (::write(socket_fd, &sync, 1) != 1)
            std::abort();

        if (::read(socket_fd, &ack, 1) != 1 || ack != '2')
        {
            std::fprintf(stderr, "FAIL: no ok ack at split %u\n", s);
            std::abort();
        }
    }

    std::printf("sender(split): all %u split points done\n", kFrameSize - 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverSplit(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    shm::FrameReader<> reader;

    constexpr uint32_t kFrameSize = shm::kMsgHeaderSize + kSplitPayloadLen;
    for (uint32_t s = 1; s < kFrameSize; ++s)
    {
        // 第一段到达：帧不完整，必须返回 -1 且不消费任何字节
        char sync = 0;
        if (::read(socket_fd, &sync, 1) != 1 || sync != 'a')
            std::abort();
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        int rc = reader.TryRecv(ch, &payload, &payload_len);
        if (rc != -1)
        {
            std::fprintf(stderr,
                         "FAIL: split=%u incomplete frame rc=%d (expect -1)\n",
                         s, rc);
            std::abort();
        }
        char ack = '1';
        if (::write(socket_fd, &ack, 1) != 1)
            std::abort();

        // 第二段到达：必须拆出完整帧
        if (::read(socket_fd, &sync, 1) != 1 || sync != 'b')
            std::abort();
        rc = reader.TryRecv(ch, &payload, &payload_len);
        if (rc != 0 || payload_len != kSplitPayloadLen)
        {
            std::fprintf(stderr, "FAIL: split=%u reassembly rc=%d len=%u\n",
                         s, rc, payload_len);
            std::abort();
        }
        for (uint32_t i = 0; i < kSplitPayloadLen; ++i)
        {
            if (static_cast<const char *>(payload)[i] != SplitByte(i))
            {
                std::fprintf(stderr, "FAIL: split=%u payload[%u]\n", s, i);
                std::abort();
            }
        }
        if (reader.LastSeq() != kMsgSeq)
        {
            std::fprintf(stderr, "FAIL: split=%u seq\n", s);
            std::abort();
        }
        ack = '2';
        if (::write(socket_fd, &ack, 1) != 1)
            std::abort();
    }

    std::printf("  PASS: all %u split points reassembled correctly\n",
                kFrameSize - 1);
    ::close(socket_fd);
}

// =========================================================================
// 测试 14（随机切分）：多帧字节流被随机大小块切分到达，按序还原
//
// 30 条变长帧（7B~3KB）拼成 ~45KB 字节流，用固定种子的随机数切成
// 1~113 字节的块逐块写入（每块一次通知，模拟不稳定到达），
// 接收方必须收到全部 30 帧，长度、内容、seq 逐一匹配。
// =========================================================================

constexpr int kStreamMsgCount = 30;

/// 第 k 条消息的 payload 长度（确定性）
uint32_t StreamMsgLen(int k)
{
    return 7u + (static_cast<uint32_t>(k) * 137u) % 2993u;
}

/// 第 k 条消息的第 i 个内容字节（确定性）
char StreamMsgByte(int k, uint32_t i)
{
    return static_cast<char>(k * 31 + static_cast<int>(i));
}

void RunSenderStream(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    // 拼接全部帧为一条字节流
    std::vector<char> stream;
    for (int k = 0; k < kStreamMsgCount; ++k)
    {
        uint32_t len = StreamMsgLen(k);
        std::vector<char> payload(len);
        for (uint32_t i = 0; i < len; ++i)
            payload[i] = StreamMsgByte(k, i);
        std::vector<char> frame(shm::kMsgHeaderSize + len);
        shm::Encode(payload.data(), len, frame.data(),
                    static_cast<uint32_t>(frame.size()),
                    static_cast<uint32_t>(k));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    // 固定种子随机切分（可复现），每块写完通知一次
    std::mt19937 rng{42};
    std::uniform_int_distribution<uint32_t> chunk_dist(1, 113);
    uint32_t offset = 0;
    int chunks = 0;
    while (offset < stream.size())
    {
        uint32_t remain = static_cast<uint32_t>(stream.size()) - offset;
        uint32_t n = chunk_dist(rng);
        if (n > remain)
            n = remain;
        if (ch.TryWrite(stream.data() + offset, n) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite chunk at offset %u\n", offset);
            std::abort();
        }
        ch.NotifyPeer();
        offset += n;
        ++chunks;
    }
    std::printf("sender(stream): %zu bytes in %d random chunks\n",
                stream.size(), chunks);

    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void RunReceiverStream(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();
    shm::FrameReader<> reader;

    int received = 0;
    while (received < kStreamMsgCount)
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        int rc = reader.TryRecv(ch, &payload, &payload_len);
        if (rc != 0)
        {
            if (rc != -1)
            {
                std::fprintf(stderr, "FAIL: stream rc=%d at msg %d\n",
                             rc, received);
                std::abort();
            }
            pollfd pfd{};
            pfd.fd     = efd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 5000) <= 0)
            {
                std::fprintf(stderr, "FAIL: poll timeout at msg %d\n", received);
                std::abort();
            }
            Channel::DrainNotify(efd);
            continue;
        }
        uint32_t expect_len = StreamMsgLen(received);
        if (payload_len != expect_len)
        {
            std::fprintf(stderr, "FAIL: msg %d len=%u expect=%u\n",
                         received, payload_len, expect_len);
            std::abort();
        }
        for (uint32_t i = 0; i < expect_len; ++i)
        {
            if (static_cast<const char *>(payload)[i]
                    != StreamMsgByte(received, i))
            {
                std::fprintf(stderr, "FAIL: msg %d payload[%u]\n",
                             received, i);
                std::abort();
            }
        }
        if (reader.LastSeq() != static_cast<uint32_t>(received))
        {
            std::fprintf(stderr, "FAIL: msg %d seq=%u\n",
                         received, reader.LastSeq());
            std::abort();
        }
        ++received;
    }

    std::printf("  PASS: %d frames reassembled in order from random chunks\n",
                received);

    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

// =========================================================================
// 辅助：fork + 运行测试对
// =========================================================================

void RunForkTest(const char *name,
                 void (*sender_fn)(int),
                 void (*receiver_fn)(int))
{
    std::printf("--- %s ---\n", name);

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        std::perror("socketpair");
        std::_Exit(1);
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        std::perror("fork");
        std::_Exit(1);
    }

    if (pid == 0)
    {
        ::close(sv[0]);
        sender_fn(sv[1]);
    }
    else
    {
        ::close(sv[1]);
        receiver_fn(sv[0]);
        ::waitpid(pid, nullptr, 0);
    }
}

}  // anonymous namespace

int main()
{
    std::printf("=== Codec test suite ===\n\n");

    // 纯内存测试
    TestEncodeDecode();
    std::printf("\n");
    TestEncodePodDecodePod();
    std::printf("\n");
    TestTagValidation();
    std::printf("\n");

    // fork 测试
    RunForkTest("Test 3: SendPod + FrameReader",
                RunSenderBasic, RunReceiverBasic);
    std::printf("\n");

    RunForkTest("Test 4: byte-by-byte stream decode",
                RunSenderByteByByte, RunReceiverByteByByte);
    std::printf("\n");

    RunForkTest("Test 5: variable-length Send + FrameReader",
                RunSenderVar, RunReceiverVar);
    std::printf("\n");

    RunForkTest("Test 7: oversized Send rejected, stream intact",
                RunSenderOversized, RunReceiverOversized);
    std::printf("\n");

    RunForkTest("Test 8: large frame zero-copy beyond BufSize",
                RunSenderLargeFrame, RunReceiverLargeFrame);
    std::printf("\n");

    RunForkTest("Test 9: -2 poisoned frame auto-discard",
                RunSenderPoison, RunReceiverPoison);
    std::printf("\n");

    TestBatchRewindCount();
    std::printf("\n");

    RunForkTest("Test 11: impossible frame length rejected (-3)",
                RunSenderBadLen, RunReceiverBadLen);
    std::printf("\n");

    RunForkTest("Test 12: poison frame eagerly skipped",
                RunSenderMixedTypes, RunReceiverMixedTypes);
    std::printf("\n");

    RunForkTest("Test 13: exhaustive two-chunk split reassembly",
                RunSenderSplit, RunReceiverSplit);
    std::printf("\n");

    RunForkTest("Test 14: random-chunk stream reassembly",
                RunSenderStream, RunReceiverStream);

    std::printf("\n=== TEST PASSED ===\n");
    return 0;
}
