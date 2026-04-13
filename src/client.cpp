/**
 * @file client.cpp
 * @brief 客户端，基于 RingChannel + EventLoop
 *
 * 通知机制：使用握手时交换的共享 eventfd。
 * Unix socket 仅用于 fd 交换和断连检测。
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>

#include <shm_ipc/client_state.hpp>
#include <shm_ipc/codec.hpp>
#include <shm_ipc/event_loop.hpp>
#include <shm_ipc/messages.hpp>

namespace {

constexpr int kTimerIntervalMs = 100;  ///< 定时器间隔（毫秒）
constexpr int kMaxTicks        = 100;  ///< 最大 tick 数

/// 全局事件循环指针，供信号处理器使用
shm_ipc::EventLoop *gLoop = nullptr;

void SignalHandler(int /*signo*/)
{
    if (gLoop)
        gLoop->Stop();
}

/**
 * @brief 连接到服务端的 Unix domain socket
 * @param path socket 文件路径
 * @return 已连接的 UniqueFd
 */
shm_ipc::UniqueFd ConnectToServer(const char *path)
{
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(sfd.Get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
        0)
        throw std::runtime_error(std::string("connect: ") + std::strerror(errno));

    return sfd;
}

}  // anonymous namespace

int main()
{
    try
    {
        struct sigaction sa
        {};
        sa.sa_handler = SignalHandler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        shm_ipc::ClientState cs;
        cs.socket_fd = ConnectToServer(shm_ipc::kDefaultSocketPath);
        std::printf("client: connected to %s\n", shm_ipc::kDefaultSocketPath);

        cs.channel    = shm_ipc::DefaultRingChannel::Connect(cs.socket_fd.Get());
        cs.notify_efd = cs.channel.NotifyReadFd();
        cs.read_buf =
            std::make_unique<char[]>(shm_ipc::DefaultRingChannel::max_msg_size);
        std::printf("client: ring channel established (capacity=%zuMB, "
                    "max_msg=%zuB, eventfd notify)\n\n",
                    shm_ipc::DefaultRingChannel::Ring::capacity / (1024 * 1024),
                    shm_ipc::DefaultRingChannel::max_msg_size);

        shm_ipc::EventLoop loop;
        gLoop = &loop;

        int write_ok   = 0;
        int write_full = 0;
        shm_ipc::PodReader<Heartbeat> hb_reader;

        // --- 定时器：向环形缓冲区写 ClientMsg POD（随机 payload 长度） ---
        std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<uint32_t> len_dist(
            16, sizeof(ClientMsg::payload));

        loop.AddTimer(kTimerIntervalMs, [&](int tfd, short /*rev*/) {
            shm_ipc::EventLoop::DrainTimerfd(tfd);

            if (++cs.tick > kMaxTicks)
            {
                loop.Stop();
                return;
            }

            ClientMsg msg{};
            msg.seq         = cs.tick;
            msg.tick        = cs.tick;
            msg.timestamp   = std::time(nullptr);
            msg.payload_len = len_dist(rng);
            std::memset(msg.payload, 'C', msg.payload_len);

            if (shm_ipc::SendPod(cs.channel, msg, static_cast<uint32_t>(cs.tick)) ==
                0)
            {
                ++write_ok;
            }
            else
            {
                ++write_full;
                std::fprintf(stderr, "client: [seq=%03d] ring full! (ok=%d, full=%d)\n",
                             cs.tick, write_ok, write_full);
            }

            if (cs.tick % 10 == 0)
            {
                std::printf("client: --- tick=%d ok=%d full=%d ring_bytes=%lu ---\n",
                            cs.tick, write_ok, write_full,
                            static_cast<unsigned long>(cs.channel.Readable()));
            }
        });

        // --- Eventfd：服务端向读环写入新数据，流式拆包 Heartbeat ---
        loop.AddFd(cs.notify_efd, [&](int efd, short /*revents*/) {
            shm_ipc::DefaultRingChannel::DrainNotify(efd);
            Heartbeat hb{};
            while (hb_reader.TryRecv(cs.channel, &hb) == 0)
            {
                std::printf("client: heartbeat [client_id=%d seq=%d ts=%ld]\n",
                            hb.client_id, hb.seq, static_cast<long>(hb.timestamp));
                ++cs.total_read;
            }
        });

        // --- Socket：仅用于断连检测 ---
        loop.AddFd(cs.socket_fd.Get(), [&](int /*fd*/, short revents) {
            if (revents & (POLLHUP | POLLERR))
            {
                std::printf("client: server disconnected\n");
                loop.Stop();
                return;
            }
            if (revents & POLLIN)
            {
                char buf{};
                auto n = ::read(cs.socket_fd.Get(), &buf, 1);
                if (n <= 0 || buf == 0)
                {
                    std::printf("client: server disconnected\n");
                    loop.Stop();
                }
            }
        });

        std::printf("client: timer=%dms, max_ticks=%d\n\n", kTimerIntervalMs,
                    kMaxTicks);
        loop.Run();

        std::printf("\nclient: done. ok=%d full=%d total_read=%d\n", write_ok,
                    write_full, cs.total_read);
        char end = 0;
        ::write(cs.socket_fd.Get(), &end, 1);
        ::usleep(100000);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "client: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
