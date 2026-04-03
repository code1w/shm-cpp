// bench_shm.cpp — Shared-memory RingChannel echo ping-pong benchmark
//
// fork() + socketpair(): child = server (echo via ring), parent = client (measure RTT).
// Uses busy-poll (no EventLoop) for lowest latency.

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

using Channel = shm_ipc::DefaultRingChannel;

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

void run_server(int socket_fd) {
    auto channel = Channel::accept(socket_fd);

    for (auto &tc : kTests) {
        int total = kWarmup + tc.rounds;
        for (int i = 0; i < total; ++i) {
            // Zero-copy read: get pointer directly into ring buffer
            const void *data{};
            uint32_t len{}, seq{};
            while (channel.peek_read(&data, &len, &seq) != 0)
                ;
            // Write directly from read ring to write ring (1 memcpy instead of 2)
            channel.try_write(data, len, seq);
            channel.commit_read(len);
        }
    }

    // Signal completion
    char end = 0;
    ::write(socket_fd, &end, 1);
    ::close(socket_fd);
    std::_Exit(0);
}

void run_client(int socket_fd) {
    auto channel = Channel::connect(socket_fd);
    auto send_buf = std::make_unique<char[]>(Channel::max_msg_size);
    auto recv_buf = std::make_unique<char[]>(Channel::max_msg_size);
    std::memset(send_buf.get(), 'B', Channel::max_msg_size);

    std::printf("=== Shared Memory Echo Benchmark ===\n");
    std::printf("%10s %10s %12s %14s %16s\n",
                "msg_size", "rounds", "total_us", "avg_rtt_ns", "throughput_MB/s");

    for (auto &tc : kTests) {
        auto msg_len = static_cast<uint32_t>(tc.msg_size);

        // Warmup
        for (int i = 0; i < kWarmup; ++i) {
            channel.try_write(send_buf.get(), msg_len, static_cast<uint32_t>(i));
            uint32_t len{}, seq{};
            while (channel.try_read(recv_buf.get(), &len, &seq) != 0)
                ;
        }

        // Benchmark
        uint64_t t0 = now_ns();
        for (int i = 0; i < tc.rounds; ++i) {
            channel.try_write(send_buf.get(), msg_len, static_cast<uint32_t>(i));
            uint32_t len{}, seq{};
            while (channel.try_read(recv_buf.get(), &len, &seq) != 0)
                ;
        }
        uint64_t elapsed_ns = now_ns() - t0;

        double total_us = static_cast<double>(elapsed_ns) / 1000.0;
        double avg_rtt_ns = static_cast<double>(elapsed_ns) / tc.rounds;
        double throughput = static_cast<double>(tc.msg_size) * 2.0 * tc.rounds
                            / (static_cast<double>(elapsed_ns) / 1e9) / (1024.0 * 1024.0);

        std::printf("%10zu %10d %12.0f %14.0f %16.2f\n",
                    tc.msg_size, tc.rounds, total_us, avg_rtt_ns, throughput);
    }

    // Wait for server to finish
    char buf{};
    ::read(socket_fd, &buf, 1);
    ::close(socket_fd);
}

} // namespace

int main() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        std::perror("socketpair");
        return 1;
    }

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
