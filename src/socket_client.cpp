/**
 * @file socket_client.cpp
 * @brief 纯 Unix socket 客户端吞吐量测试
 *
 * 与 client --bench 使用相同的协议和输出格式，
 * 但数据通过 Unix socket 传输而非共享内存，用于性能对比。
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <shm_ipc/bench_common.hpp>
#include <shm_ipc/codec.hpp>

namespace {

constexpr const char *kSocketPath = "/tmp/shm-ipc-sock-bench.socket";

void WriteAll(int fd, const void *buf, std::size_t len)
{
    auto *p = static_cast<const char *>(buf);
    while (len > 0)
    {
        auto n = ::write(fd, p, len);
        if (n <= 0)
        {
            std::perror("write");
            std::_Exit(1);
        }
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
}

shm_ipc::UniqueFd ConnectToServer(const char *path)
{
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(sfd.Get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("connect: ") + std::strerror(errno));

    return sfd;
}

void RunBench()
{
    auto sfd = ConnectToServer(kSocketPath);
    std::printf("socket_client(bench): connected\n\n");

    // 扩大 socket 缓冲区
    int buf_size = 4 * 1024 * 1024;
    ::setsockopt(sfd.Get(), SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    shm_ipc::PrintBenchHeader();

    for (auto &tc : shm_ipc::kBenchCases)
    {
        // 通知 server 本轮参数
        shm_ipc::BenchCmd cmd{tc.payload_size, tc.rounds};
        WriteAll(sfd.Get(), &cmd, sizeof(cmd));

        // 构造 payload = BenchPayloadHeader + 填充字节
        uint32_t body_len = static_cast<uint32_t>(sizeof(shm_ipc::BenchPayloadHeader))
                          + tc.payload_size;
        std::vector<char> body(body_len);
        std::memset(body.data() + sizeof(shm_ipc::BenchPayloadHeader), 'B',
                    tc.payload_size);

        // 编码缓冲区
        uint32_t frame_size = shm_ipc::BenchFrameSize(tc.payload_size);
        std::vector<char> frame_buf(frame_size);

        uint64_t t0 = shm_ipc::NowNs();
        for (int32_t i = 0; i < tc.rounds; ++i)
        {
            shm_ipc::BenchPayloadHeader hdr{i};
            std::memcpy(body.data(), &hdr, sizeof(hdr));
            uint32_t n = shm_ipc::Encode(shm_ipc::kBenchTag,
                                          body.data(), body_len,
                                          frame_buf.data(), frame_size,
                                          static_cast<uint32_t>(i));
            WriteAll(sfd.Get(), frame_buf.data(), n);
        }

        // 发结束标记
        shm_ipc::BenchPayloadHeader end_hdr{-1};
        char end_frame[64];
        uint32_t n = shm_ipc::Encode(shm_ipc::kBenchTag,
                                      &end_hdr, sizeof(end_hdr),
                                      end_frame, sizeof(end_frame), 0);
        WriteAll(sfd.Get(), end_frame, n);

        // 等 server ack
        char ack = 0;
        (void)::read(sfd.Get(), &ack, 1);
        uint64_t elapsed = shm_ipc::NowNs() - t0;

        shm_ipc::PrintBenchRow(tc.payload_size, tc.rounds, elapsed);
    }

    // 通知 server 结束
    shm_ipc::BenchCmd end{0, 0};
    WriteAll(sfd.Get(), &end, sizeof(end));

    std::printf("\nsocket_client(bench): done\n");
}

}  // anonymous namespace

int main()
{
    try
    {
        RunBench();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "socket_client: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
