// bench_socket.cpp — Unix socket echo ping-pong benchmark
//
// fork() + socketpair(): child = server (echo), parent = client (measure RTT).

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

// Fully read/write `len` bytes, handling partial transfers.
void write_all(int fd, const void *buf, std::size_t len) {
    auto *p = static_cast<const char *>(buf);
    while (len > 0) {
        auto n = ::write(fd, p, len);
        if (n <= 0) std::_Exit(1);
        p += n;
        len -= static_cast<std::size_t>(n);
    }
}

void read_all(int fd, void *buf, std::size_t len) {
    auto *p = static_cast<char *>(buf);
    while (len > 0) {
        auto n = ::read(fd, p, len);
        if (n <= 0) std::_Exit(1);
        p += n;
        len -= static_cast<std::size_t>(n);
    }
}

uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

struct TestCase {
    std::size_t msg_size;
    int         rounds;
};

constexpr std::array<TestCase, 6> kTests = {{
    {     64, 100000},
    {    256, 100000},
    {   1024,  50000},
    {   4096,  50000},
    {  65536,  10000},
    { 524288,   5000},
}};

constexpr int kWarmup = 1000;

void run_server(int fd) {
    auto buf = std::make_unique<char[]>(524288);

    for (auto &tc : kTests) {
        int total = kWarmup + tc.rounds;
        for (int i = 0; i < total; ++i) {
            read_all(fd, buf.get(), tc.msg_size);
            write_all(fd, buf.get(), tc.msg_size);
        }
    }
    ::close(fd);
    std::_Exit(0);
}

void run_client(int fd) {
    auto send_buf = std::make_unique<char[]>(524288);
    auto recv_buf = std::make_unique<char[]>(524288);
    std::memset(send_buf.get(), 'A', 524288);

    std::printf("=== Unix Socket Echo Benchmark ===\n");
    std::printf("%10s %10s %12s %14s %16s\n",
                "msg_size", "rounds", "total_us", "avg_rtt_ns", "throughput_MB/s");

    for (auto &tc : kTests) {
        // Warmup
        for (int i = 0; i < kWarmup; ++i) {
            write_all(fd, send_buf.get(), tc.msg_size);
            read_all(fd, recv_buf.get(), tc.msg_size);
        }

        // Benchmark
        uint64_t t0 = now_ns();
        for (int i = 0; i < tc.rounds; ++i) {
            write_all(fd, send_buf.get(), tc.msg_size);
            read_all(fd, recv_buf.get(), tc.msg_size);
        }
        uint64_t elapsed_ns = now_ns() - t0;

        double total_us = static_cast<double>(elapsed_ns) / 1000.0;
        double avg_rtt_ns = static_cast<double>(elapsed_ns) / tc.rounds;
        // Each round transfers msg_size * 2 (send + echo)
        double throughput = static_cast<double>(tc.msg_size) * 2.0 * tc.rounds
                            / (static_cast<double>(elapsed_ns) / 1e9) / (1024.0 * 1024.0);

        std::printf("%10zu %10d %12.0f %14.0f %16.2f\n",
                    tc.msg_size, tc.rounds, total_us, avg_rtt_ns, throughput);
    }

    ::close(fd);
}

} // namespace

int main() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        std::perror("socketpair");
        return 1;
    }

    // Increase socket buffer sizes for larger messages
    int buf_size = 1024 * 1024;
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }

    if (pid == 0) {
        // Child = server
        ::close(sv[0]);
        run_server(sv[1]);
    } else {
        // Parent = client
        ::close(sv[1]);
        run_client(sv[0]);
        ::waitpid(pid, nullptr, 0);
    }

    return 0;
}
