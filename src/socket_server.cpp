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

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <shm_ipc/bench_common.hpp>
#include <shm_ipc/codec.hpp>
#include <shm_ipc/pod_codec.hpp>

namespace {

constexpr const char* kSocketPath = "/tmp/shm-ipc-sock-bench.socket";

void ReadAll(int fd, void* buf, std::size_t len)
{
    auto* p = static_cast<char*>(buf);
    while (len > 0)
    {
        auto n = ::read(fd, p, len);
        if (n <= 0)
        {
            std::perror("read");
            std::_Exit(1);
        }
        p += n;
        len -= static_cast<std::size_t>(n);
    }
}

bool CreateListenSocket(const char* path, shm::UniqueFd* out)
{
    int sys_errno     = 0;
    shm::ErrorCode ec = shm::ListenUnixSocket(path, out, 1, &sys_errno);
    if (ec != shm::ErrorCode::kOk)
    {
        std::fprintf(stderr, "socket_server: listen failed: %s\n", shm::ErrorCodeMessage(ec, sys_errno).c_str());
        return false;
    }
    return true;
}

bool RunBench()
{
    shm::UniqueFd listen_fd;
    if (!CreateListenSocket(kSocketPath, &listen_fd))
        return false;
    std::printf("socket_server(bench): waiting for client on %s ...\n", kSocketPath);

    shm::UniqueFd cfd{::accept(listen_fd.Get(), nullptr, nullptr)};
    if (!cfd)
    {
        std::perror("accept");
        return false;
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

        // 命令来自对端：payload_size 超过 16MB 或 rounds 为负视为协议违规。
        // 不校验时 payload_size 接近 UINT32_MAX 会令 max_body 回绕成极小值
        if (cmd.payload_size > (16u << 20) || cmd.rounds < 0)
        {
            std::fprintf(stderr,
                         "socket_server: invalid cmd (payload_size=%u rounds=%d)"
                         ", disconnect\n",
                         cmd.payload_size, cmd.rounds);
            return false;
        }

        // 接收缓冲区（MsgHeader 之后的 body 部分）
        uint32_t max_body = shm::kTagSize + static_cast<uint32_t>(sizeof(shm::BenchPayloadHeader)) + cmd.payload_size;
        std::vector<char> body_buf(max_body);

        int32_t received = 0;
        uint64_t t0      = shm::NowNs();

        bool got_end = false;
        while (!got_end)
        {
            // 先读 MsgHeader
            shm::MsgHeader hdr{};
            ReadAll(cfd.Get(), &hdr, sizeof(hdr));

            // 再读 body（tag + payload）。len 完全由对端控制，必须与按
            // cmd.payload_size 分配的缓冲比对，否则越界写堆内存
            // （/tmp 下的 socket 任意本地进程可连，可触发）
            if (hdr.len > body_buf.size())
            {
                std::fprintf(stderr,
                             "socket_server: frame len %u exceeds buffer %zu"
                             " (payload_size=%u), disconnect\n",
                             hdr.len, body_buf.size(), cmd.payload_size);
                return false;
            }
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
    return true;
}

}  // anonymous namespace

int main()
{
    // 对端中途退出时 write 会触发 SIGPIPE，忽略之以走 EPIPE 错误路径
    ::signal(SIGPIPE, SIG_IGN);

    return RunBench() ? 0 : EXIT_FAILURE;
}
