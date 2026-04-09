/**
 * @file test_codec.cpp
 * @brief PodReader 逐字节分片解码测试
 *
 * 验证：发送端将一个大 POD（ClientMsg, ~4KB）编码后逐字节写入 ring，
 * 每写 1 字节就 NotifyPeer()；接收端被唤醒 N 次，每次调用
 * PodReader::TryRecv，直到最后一字节到达时成功解码。
 *
 * 使用 fork + socketpair，与 bench_shm.cpp 相同的进程隔离方式。
 */

#include <shm_ipc/codec.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/ring_channel.hpp>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Channel = shm_ipc::DefaultRingChannel;

constexpr uint32_t kMagicPayloadLen = 2048;
constexpr char     kFillByte        = 'X';
constexpr int32_t  kSeq             = 42;
constexpr int32_t  kTick            = 7;
constexpr int64_t  kTimestamp       = 1234567890;

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
    assert(msg.seq == kSeq);
    assert(msg.tick == kTick);
    assert(msg.timestamp == kTimestamp);
    assert(msg.payload_len == kMagicPayloadLen);
    for (uint32_t i = 0; i < kMagicPayloadLen; ++i)
        assert(msg.payload[i] == kFillByte);
    std::printf("  PASS: all fields match\n");
}

/**
 * @brief 发送端：将 codec 帧逐字节写入 ring，每字节一条消息 + 一次通知
 */
void RunSender(int socket_fd)
{
    auto ch = Channel::Accept(socket_fd);

    ClientMsg msg = MakeTestMsg();

    // 编码为 codec 帧
    constexpr uint32_t frame_size = shm_ipc::kTagSize + sizeof(ClientMsg);
    char frame[frame_size];
    uint32_t n = shm_ipc::Encode(msg, frame, frame_size);
    assert(n == frame_size);

    std::printf("sender: frame_size=%u, sending byte-by-byte\n", frame_size);

    // 逐字节写入 ring，每字节一条 ring 消息
    for (uint32_t i = 0; i < frame_size; ++i)
    {
        int rc = ch.TryWrite(&frame[i], 1, i);
        assert(rc == 0);
        ch.NotifyPeer();
    }

    std::printf("sender: all %u bytes sent\n", frame_size);

    // 等待接收端确认完成
    char ack = 0;
    ::read(socket_fd, &ack, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

/**
 * @brief 接收端：poll eventfd，每次唤醒调 TryRecv，统计唤醒次数
 */
void RunReceiver(int socket_fd)
{
    auto ch = Channel::Connect(socket_fd);
    int efd = ch.NotifyReadFd();

    shm_ipc::PodReader<ClientMsg> reader;
    ClientMsg out{};

    int wakeups       = 0;
    int recv_attempts = 0;
    bool decoded      = false;

    constexpr uint32_t frame_size = shm_ipc::kTagSize + sizeof(ClientMsg);

    std::printf("receiver: expecting frame_size=%u, polling...\n", frame_size);

    while (!decoded)
    {
        // poll 等待 eventfd 可读
        pollfd pfd{};
        pfd.fd     = efd;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, 5000);  // 5s 超时
        if (pr <= 0)
        {
            std::fprintf(stderr, "receiver: poll timeout/error, wakeups=%d\n", wakeups);
            assert(false && "poll timeout");
        }

        Channel::DrainNotify(efd);
        ++wakeups;

        // 可能一次唤醒后有多字节可读，循环尝试
        ++recv_attempts;
        if (reader.TryRecv(ch, &out) == 0)
        {
            decoded = true;
        }
    }

    std::printf("receiver: decoded after %d wakeups, %d TryRecv calls\n",
                wakeups, recv_attempts);
    std::printf("receiver: buffered residual=%u\n", reader.Buffered());

    VerifyMsg(out);

    // 通知发送端可退出
    char ack = 1;
    ::write(socket_fd, &ack, 1);
    ::close(socket_fd);
}

}  // anonymous namespace

int main()
{
    std::printf("=== PodReader byte-by-byte decode test ===\n");
    std::printf("ClientMsg sizeof=%zu, codec frame=%zu\n\n",
                sizeof(ClientMsg), shm_ipc::kTagSize + sizeof(ClientMsg));

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        std::perror("socketpair");
        return 1;
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        std::perror("fork");
        return 1;
    }

    if (pid == 0)
    {
        ::close(sv[0]);
        RunSender(sv[1]);
    }
    else
    {
        ::close(sv[1]);
        RunReceiver(sv[0]);
        ::waitpid(pid, nullptr, 0);
    }

    std::printf("\n=== TEST PASSED ===\n");
    return 0;
}
