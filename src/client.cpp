/**
 * @file client.cpp
 * @brief 客户端，基于 RingChannel + EventLoop
 *
 * 通知机制：使用握手时交换的共享 eventfd。
 * Unix socket 仅用于 fd 交换和断连检测。
 */

#include <shm_ipc/ring_channel.hpp>
#include <shm_ipc/event_loop.hpp>

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr int kTimerIntervalMs = 100; ///< 定时器间隔（毫秒）
constexpr int kMaxTicks        = 100; ///< 最大 tick 数

/// 全局事件循环指针，供信号处理器使用
shm_ipc::EventLoop* gLoop = nullptr;

void SignalHandler(int /*signo*/)
{
    if (gLoop) gLoop->Stop();
}

/**
 * @brief 连接到服务端的 Unix domain socket
 * @param path socket 文件路径
 * @return 已连接的 UniqueFd
 */
shm_ipc::UniqueFd ConnectToServer(const char* path)
{
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(sfd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("connect: ") + std::strerror(errno));

    return sfd;
}

} // anonymous namespace

int main()
{
    try
    {
        struct sigaction sa{};
        sa.sa_handler = SignalHandler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        auto sfd = ConnectToServer(shm_ipc::kDefaultSocketPath);
        std::printf("client: connected to %s\n", shm_ipc::kDefaultSocketPath);

        auto channel = shm_ipc::DefaultRingChannel::Connect(sfd.Get());
        std::printf("client: ring channel established (capacity=%zuMB, max_msg=%zuB, eventfd notify)\n\n",
                    shm_ipc::DefaultRingChannel::Ring::capacity / (1024 * 1024),
                    shm_ipc::DefaultRingChannel::max_msg_size);

        shm_ipc::EventLoop loop;
        gLoop = &loop;

        int tick       = 0;
        int write_ok   = 0;
        int write_full = 0;
        int socket_fd  = sfd.Get();

        auto read_buf = std::make_unique<char[]>(shm_ipc::DefaultRingChannel::max_msg_size);

        // --- 定时器：向环形缓冲区写消息，通过 eventfd 通知 ---
        loop.AddTimer(kTimerIntervalMs, [&](int tfd, short /*rev*/) {
            shm_ipc::EventLoop::DrainTimerfd(tfd);

            if (++tick > kMaxTicks)
            {
                loop.Stop();
                return;
            }

            auto              now = std::time(nullptr);
            std::array<char, 32> ts{};
            std::strftime(ts.data(), ts.size(), "%H:%M:%S", std::localtime(&now));

            std::array<char, 4096> msg{};
            int msg_len = std::snprintf(msg.data(), msg.size(),
                "[client seq=%03d time=%s] tick=%d", tick, ts.data(), tick);

            if (channel.TryWrite(msg.data(),
                    static_cast<uint32_t>(msg_len + 1), tick) == 0)
            {
                ++write_ok;
                channel.NotifyPeer();
            }
            else
            {
                ++write_full;
                std::fprintf(stderr, "client: [seq=%03d] ring full! (ok=%d, full=%d)\n",
                             tick, write_ok, write_full);
            }

            if (tick % 10 == 0)
            {
                std::printf("client: --- tick=%d ok=%d full=%d ring_bytes=%lu ---\n",
                            tick, write_ok, write_full,
                            static_cast<unsigned long>(channel.Readable()));
            }
        });

        // --- Eventfd：服务端向读环写入新数据 ---
        loop.AddFd(channel.NotifyReadFd(), [&](int efd, short /*revents*/) {
            shm_ipc::DefaultRingChannel::DrainNotify(efd);
            uint32_t len = 0;
            uint32_t seq = 0;
            while (channel.TryRead(read_buf.get(), &len, &seq) == 0)
            {
                std::printf("client: server [seq=%03u len=%u]: %.*s\n",
                            seq, len, static_cast<int>(len), read_buf.get());
            }
        });

        // --- Socket：仅用于断连检测 ---
        loop.AddFd(socket_fd, [&](int /*fd*/, short revents) {
            if (revents & (POLLHUP | POLLERR))
            {
                std::printf("client: server disconnected\n");
                loop.Stop();
                return;
            }
            if (revents & POLLIN)
            {
                char buf{};
                auto n = ::read(socket_fd, &buf, 1);
                if (n <= 0 || buf == 0)
                {
                    std::printf("client: server disconnected\n");
                    loop.Stop();
                }
            }
        });

        std::printf("client: timer=%dms, max_ticks=%d\n\n", kTimerIntervalMs, kMaxTicks);
        loop.Run();

        std::printf("\nclient: done. ok=%d full=%d\n", write_ok, write_full);
        char end = 0;
        ::write(socket_fd, &end, 1);
        ::usleep(100000);

    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "client: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
