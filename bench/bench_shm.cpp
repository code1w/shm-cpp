/**
 * @file bench_shm.cpp
 * @brief 共享内存 RingChannel 回声 ping-pong 基准测试
 *
 * 使用 fork() + socketpair()：子进程为服务端（通过环形缓冲区回声），
 * 父进程为客户端（测量 RTT）。采用忙轮询以获得最低延迟。
 */

#include <shm_ipc/ring_channel.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

using Channel = shm::DefaultRingChannel;

/** @brief 返回单调时钟的纳秒时间戳 */
uint64_t NowNs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

/** @brief 单个测试用例 */
struct TestCase
{
    std::size_t msg_size; ///< 消息大小（字节）
    int         rounds;   ///< 测试轮次
};

constexpr std::array<TestCase, 6> kTests = {{
    {     64, 100000},
    {    256, 100000},
    {   1024,  50000},
    {   4096,  50000},
    {  65536,  10000},
    { 524288,   5000},
}};

constexpr int kWarmup = 1000; ///< 预热轮次

/**
 * @brief 服务端运行逻辑：读取消息后立即回写
 * @param socket_fd 与客户端共享的 socket fd
 */
void RunServer(int socket_fd)
{
    auto channel = Channel::Accept(socket_fd);
    auto buf     = std::make_unique<char[]>(Channel::max_write_size);

    for (auto& tc : kTests)
    {
        auto msg_len = static_cast<uint32_t>(tc.msg_size);
        int total = kWarmup + tc.rounds;
        for (int i = 0; i < total; ++i)
        {
            // 等待完整消息到达
            while (channel.ReadExact(buf.get(), msg_len) != 0)
                ;
            // 回写
            while (channel.TryWrite(buf.get(), msg_len) != 0)
                ;
        }
    }

    // 通知客户端完成
    char end = 0;
    ::write(socket_fd, &end, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

/**
 * @brief 客户端运行逻辑：发送消息并等待回声，统计 RTT
 * @param socket_fd 与服务端共享的 socket fd
 */
void RunClient(int socket_fd)
{
    auto channel  = Channel::Connect(socket_fd);
    auto send_buf = std::make_unique<char[]>(Channel::max_write_size);
    auto recv_buf = std::make_unique<char[]>(Channel::max_write_size);
    std::memset(send_buf.get(), 'B', Channel::max_write_size);

    std::printf("=== Shared Memory Echo Benchmark ===\n");
    std::printf("%10s %10s %12s %14s %16s\n",
                "msg_size", "rounds", "total_us", "avg_rtt_ns", "throughput_MB/s");

    for (auto& tc : kTests)
    {
        auto msg_len = static_cast<uint32_t>(tc.msg_size);

        // 预热
        for (int i = 0; i < kWarmup; ++i)
        {
            channel.TryWrite(send_buf.get(), msg_len);
            while (channel.ReadExact(recv_buf.get(), msg_len) != 0)
                ;
        }

        // 正式测试
        uint64_t t0 = NowNs();
        for (int i = 0; i < tc.rounds; ++i)
        {
            channel.TryWrite(send_buf.get(), msg_len);
            while (channel.ReadExact(recv_buf.get(), msg_len) != 0)
                ;
        }
        uint64_t elapsed_ns = NowNs() - t0;

        double total_us    = static_cast<double>(elapsed_ns) / 1000.0;
        double avg_rtt_ns  = static_cast<double>(elapsed_ns) / tc.rounds;
        double throughput  = static_cast<double>(tc.msg_size) * 2.0 * tc.rounds
                           / (static_cast<double>(elapsed_ns) / 1e9) / (1024.0 * 1024.0);

        std::printf("%10zu %10d %12.0f %14.0f %16.2f\n",
                    tc.msg_size, tc.rounds, total_us, avg_rtt_ns, throughput);
    }

    // 等待服务端完成
    char buf{};
    ::read(socket_fd, &buf, 1);
    ::close(socket_fd);
}

} // anonymous namespace

int main()
{
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
        // 子进程 = 服务端
        ::close(sv[0]);
        RunServer(sv[1]);
    }
    else
    {
        // 父进程 = 客户端
        ::close(sv[1]);
        RunClient(sv[0]);
        ::waitpid(pid, nullptr, 0);
    }

    return 0;
}
