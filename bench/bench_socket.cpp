/**
 * @file bench_socket.cpp
 * @brief Unix socket 回声 ping-pong 基准测试
 *
 * 使用 fork() + socketpair()：子进程为服务端（回声），
 * 父进程为客户端（测量 RTT）。
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

/**
 * @brief 完整写入 len 字节，处理短写
 * @param fd  目标文件描述符
 * @param buf 数据缓冲区
 * @param len 写入字节数
 */
void WriteAll(int fd, const void* buf, std::size_t len)
{
    auto* p = static_cast<const char*>(buf);
    while (len > 0)
    {
        auto n = ::write(fd, p, len);
        if (n <= 0) std::_Exit(1);
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
}

/**
 * @brief 完整读取 len 字节，处理短读
 * @param fd  源文件描述符
 * @param buf 输出缓冲区
 * @param len 读取字节数
 */
void ReadAll(int fd, void* buf, std::size_t len)
{
    auto* p = static_cast<char*>(buf);
    while (len > 0)
    {
        auto n = ::read(fd, p, len);
        if (n <= 0) std::_Exit(1);
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
}

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
 * @brief 服务端运行逻辑：接收后原样回写
 * @param fd socket 文件描述符
 */
void RunServer(int fd)
{
    auto buf = std::make_unique<char[]>(524288);

    for (auto& tc : kTests)
    {
        int total = kWarmup + tc.rounds;
        for (int i = 0; i < total; ++i)
        {
            ReadAll(fd, buf.get(), tc.msg_size);
            WriteAll(fd, buf.get(), tc.msg_size);
        }
    }
    ::close(fd);
    std::_Exit(0);
}

/**
 * @brief 客户端运行逻辑：发送消息并等待回声，统计 RTT
 * @param fd socket 文件描述符
 */
void RunClient(int fd)
{
    auto send_buf = std::make_unique<char[]>(524288);
    auto recv_buf = std::make_unique<char[]>(524288);
    std::memset(send_buf.get(), 'A', 524288);

    std::printf("=== Unix Socket Echo Benchmark ===\n");
    std::printf("%10s %10s %12s %14s %16s\n",
                "msg_size", "rounds", "total_us", "avg_rtt_ns", "throughput_MB/s");

    for (auto& tc : kTests)
    {
        // 预热
        for (int i = 0; i < kWarmup; ++i)
        {
            WriteAll(fd, send_buf.get(), tc.msg_size);
            ReadAll(fd, recv_buf.get(), tc.msg_size);
        }

        // 正式测试
        uint64_t t0 = NowNs();
        for (int i = 0; i < tc.rounds; ++i)
        {
            WriteAll(fd, send_buf.get(), tc.msg_size);
            ReadAll(fd, recv_buf.get(), tc.msg_size);
        }
        uint64_t elapsed_ns = NowNs() - t0;

        double total_us   = static_cast<double>(elapsed_ns) / 1000.0;
        double avg_rtt_ns = static_cast<double>(elapsed_ns) / tc.rounds;
        // 每轮传输 msg_size * 2（发送 + 回声）
        double throughput = static_cast<double>(tc.msg_size) * 2.0 * tc.rounds
                          / (static_cast<double>(elapsed_ns) / 1e9) / (1024.0 * 1024.0);

        std::printf("%10zu %10d %12.0f %14.0f %16.2f\n",
                    tc.msg_size, tc.rounds, total_us, avg_rtt_ns, throughput);
    }

    ::close(fd);
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

    // 为较大消息扩大 socket 缓冲区
    int buf_size = 1024 * 1024;
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

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
