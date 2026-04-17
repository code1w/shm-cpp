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
 *
 * 使用 fork + socketpair，与 bench_shm.cpp 相同的进程隔离方式。
 */

#include <shm_ipc/codec.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/pod_codec.hpp>
#include <shm_ipc/ring_channel.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
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
    if (shm::EncodePod(hb, buf, frame_size, send_seq) != frame_size)
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
    if (!shm::DecodePod<Heartbeat>(pod_data, pod_len, &out))
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
    if (shm::SendPod(ch, msg, kMsgSeq) != 0)
    {
        std::fprintf(stderr, "FAIL: SendPod\n");
        std::abort();
    }

    std::printf("sender(basic): sent 1 ClientMsg via SendPod\n");

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
    if (!shm::DecodePod<ClientMsg>(pod_data, pod_len, &out))
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
    if (shm::EncodePod(msg, frame, frame_size, kMsgSeq) != frame_size)
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
    if (!shm::DecodePod<ClientMsg>(pod_data, pod_len, &out))
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

    // fork 测试
    RunForkTest("Test 3: SendPod + FrameReader",
                RunSenderBasic, RunReceiverBasic);
    std::printf("\n");

    RunForkTest("Test 4: byte-by-byte stream decode",
                RunSenderByteByByte, RunReceiverByteByByte);
    std::printf("\n");

    RunForkTest("Test 5: variable-length Send + FrameReader",
                RunSenderVar, RunReceiverVar);

    std::printf("\n=== TEST PASSED ===\n");
    return 0;
}
