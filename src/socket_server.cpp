/**
 * @file socket_server.cpp
 * @brief 纯 Unix socket 服务端吞吐量测试
 *
 * 与 server --bench 使用相同的协议和输出格式，
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
#include <shm_ipc/pod_codec.hpp>

namespace {

constexpr const char *kSocketPath = "/tmp/shm-ipc-sock-bench.socket";

void ReadAll(int fd, void *buf, std::size_t len)
{
    auto *p = static_cast<char *>(buf);
    while (len > 0)
    {
        auto n = ::read(fd, p, len);
        if (n <= 0)
        {
            std::perror("read");
            std::_Exit(1);
        }
        p   += n;
        len -= static_cast<std::size_t>(n);
    }
}

shm::UniqueFd CreateListenSocket(const char *path)
{
    shm::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    ::unlink(path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::bind(sfd.Get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(sfd.Get(), 1) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));

    return sfd;
}

void RunBench()
{
    auto listen_fd = CreateListenSocket(kSocketPath);
    std::printf("socket_server(bench): waiting for client on %s ...\n", kSocketPath);

    shm::UniqueFd cfd{::accept(listen_fd.Get(), nullptr, nullptr)};
    if (!cfd)
    {
        std::perror("accept");
        return;
    }

    // 扩大 socket 缓冲区
    int buf_size = 4 * 1024 * 1024;
    ::setsockopt(cfd.Get(), SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    std::printf("socket_server(bench): client connected\n\n");

    shm::PrintBenchHeader();

    for (;;)
    {
        shm::BenchCmd cmd{};
        ReadAll(cfd.Get(), &cmd, sizeof(cmd));
        if (cmd.rounds == 0)
            break;

        // 接收缓冲区（MsgHeader 之后的 body 部分）
        uint32_t max_body = shm::kTagSize
                          + static_cast<uint32_t>(sizeof(shm::BenchPayloadHeader))
                          + cmd.payload_size;
        std::vector<char> body_buf(max_body);

        int32_t received = 0;
        uint64_t t0 = shm::NowNs();

        bool got_end = false;
        while (!got_end)
        {
            // 先读 MsgHeader
            shm::MsgHeader hdr{};
            ReadAll(cfd.Get(), &hdr, sizeof(hdr));

            // 再读 body（tag + payload）
            ReadAll(cfd.Get(), body_buf.data(), hdr.len);

            // 解析 tag
            uint32_t tag = 0;
            std::memcpy(&tag, body_buf.data(), shm::kTagSize);

            // 解析 BenchPayloadHeader
            if (hdr.len >= shm::kTagSize + sizeof(shm::BenchPayloadHeader))
            {
                shm::BenchPayloadHeader ph{};
                std::memcpy(&ph, body_buf.data() + shm::kTagSize, sizeof(ph));
                if (ph.seq == -1)
                {
                    got_end = true;
                    break;
                }
            }
            ++received;
        }
        uint64_t elapsed = shm::NowNs() - t0;

        // 发 ack
        char ack = 1;
        (void)::write(cfd.Get(), &ack, 1);

        shm::PrintBenchRow(cmd.payload_size, received, elapsed);
    }

    std::printf("\nsocket_server(bench): done\n");
    ::unlink(kSocketPath);
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
        std::fprintf(stderr, "socket_server: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
