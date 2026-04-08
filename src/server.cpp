/**
 * @file server.cpp
 * @brief 多客户端服务端，基于 RingChannel + EventLoop
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
#include <unordered_map>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr int kTimerIntervalMs = 300; ///< 定时器间隔（毫秒）
constexpr int kMaxTicks        = 40;  ///< 每客户端最大 tick 数
constexpr int kMaxClients      = 16;  ///< 最大并发客户端数

using Channel = shm_ipc::DefaultRingChannel;

/** @brief 单个客户端的运行状态 */
struct ClientState
{
    int                     id         = 0;  ///< 客户端 ID
    shm_ipc::UniqueFd       socket_fd;       ///< 客户端 socket fd
    Channel                 channel;          ///< 双向共享内存通道
    int                     timer_fd   = -1; ///< 定时器 fd
    int                     notify_efd = -1; ///< 轮询客户端通知的 eventfd
    int                     tick       = 0;  ///< 当前 tick 计数
    int                     total_read = 0;  ///< 累计读取消息数
    std::unique_ptr<char[]> read_buf;         ///< 消息读取缓冲区
};

/// 全局事件循环指针，供信号处理器使用
shm_ipc::EventLoop* gLoop = nullptr;

void SignalHandler(int /*signo*/)
{
    if (gLoop) gLoop->Stop();
}

/**
 * @brief 创建并绑定 Unix domain socket 监听端
 * @param path socket 文件路径
 * @return 已处于 listen 状态的 UniqueFd
 */
shm_ipc::UniqueFd CreateListenSocket(const char* path)
{
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    ::unlink(path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::bind(sfd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(sfd.Get(), 5) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));

    return sfd;
}

/**
 * @brief 从事件循环中移除客户端的所有关联 fd
 * @param loop      事件循环
 * @param clients   客户端状态映射表
 * @param client_fd 客户端 socket fd（作为 map key）
 * @param csp       客户端状态指针
 */
void RemoveClient(shm_ipc::EventLoop& loop,
                  std::unordered_map<int, std::unique_ptr<ClientState>>& clients,
                  int client_fd, ClientState* csp)
{
    loop.RemoveTimer(csp->timer_fd);
    loop.RemoveFd(csp->notify_efd);
    loop.RemoveFd(client_fd);
    clients.erase(client_fd);
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

        shm_ipc::EventLoop loop;
        gLoop = &loop;

        std::unordered_map<int, std::unique_ptr<ClientState>> clients;
        int next_client_id = 1;

        auto listen_fd = CreateListenSocket(shm_ipc::kDefaultSocketPath);
        std::printf("server: listening on %s (max %d clients, ring=%zuMB, eventfd notify)\n",
                    shm_ipc::kDefaultSocketPath, kMaxClients,
                    Channel::Ring::capacity / (1024 * 1024));

        // --- 接受连接回调 ---
        loop.AddFd(listen_fd.Get(), [&](int lfd, short /*revents*/) {
            if (static_cast<int>(clients.size()) >= kMaxClients)
            {
                std::fprintf(stderr, "server: max clients reached, rejecting\n");
                shm_ipc::UniqueFd rejected{::accept(lfd, nullptr, nullptr)};
                return;
            }

            shm_ipc::UniqueFd cfd{::accept(lfd, nullptr, nullptr)};
            if (!cfd) return;

            int cid = next_client_id++;
            std::printf("server: client #%d connected (fd=%d)\n", cid, cfd.Get());

            auto cs      = std::make_unique<ClientState>();
            cs->id       = cid;
            cs->channel  = Channel::Accept(cfd.Get());
            cs->read_buf = std::make_unique<char[]>(Channel::max_msg_size);
            std::printf("server: client #%d ring channel established (eventfd notify)\n", cid);

            int         client_fd = cfd.Get();
            cs->socket_fd         = std::move(cfd);
            cs->notify_efd        = cs->channel.NotifyReadFd();

            ClientState* csp = cs.get();

            // --- 每客户端定时器：发送响应 + 批量读 ---
            int tfd = loop.AddTimer(kTimerIntervalMs, [&loop, &clients, client_fd, csp](int timer_fd, short /*rev*/) {
                shm_ipc::EventLoop::DrainTimerfd(timer_fd);

                if (++csp->tick > kMaxTicks)
                {
                    std::printf("server: client #%d max ticks reached\n", csp->id);
                    RemoveClient(loop, clients, client_fd, csp);
                    return;
                }

                auto              now = std::time(nullptr);
                std::array<char, 32> ts{};
                std::strftime(ts.data(), ts.size(), "%H:%M:%S", std::localtime(&now));

                std::array<char, 4096> msg{};
                int msg_len = std::snprintf(msg.data(), msg.size(),
                    "[server seq=%03d time=%s client=#%d] tick=%d",
                    csp->tick, ts.data(), csp->id, csp->tick);

                if (csp->channel.TryWrite(msg.data(),
                        static_cast<uint32_t>(msg_len + 1), csp->tick) == 0)
                {
                    csp->channel.NotifyPeer();
                }

                // 批量读取客户端消息
                uint32_t len = 0;
                uint32_t seq = 0;
                int      batch = 0;
                while (csp->channel.TryRead(csp->read_buf.get(), &len, &seq) == 0)
                {
                    std::printf("server: client #%d [seq=%03u len=%u]: %.*s\n",
                                csp->id, seq, len,
                                static_cast<int>(len), csp->read_buf.get());
                    ++csp->total_read;
                    ++batch;
                }
                if (batch > 0)
                {
                    std::printf("server: client #%d batch=%d total=%d\n",
                                csp->id, batch, csp->total_read);
                }
            });
            csp->timer_fd = tfd;

            // --- Eventfd：客户端写入新数据（仅排空，由定时器批量读） ---
            loop.AddFd(csp->notify_efd, [csp](int efd, short /*revents*/) {
                Channel::DrainNotify(efd);
                // 数据将在下次定时器 tick 批量读取
            });

            // --- Socket：仅用于断连检测 ---
            loop.AddFd(client_fd, [&loop, &clients, client_fd, csp](int /*fd*/, short revents) {
                if (revents & (POLLHUP | POLLERR))
                {
                    std::printf("server: client #%d disconnected (HUP/ERR)\n", csp->id);
                    RemoveClient(loop, clients, client_fd, csp);
                    return;
                }
                if (revents & POLLIN)
                {
                    char buf{};
                    auto n = ::read(client_fd, &buf, 1);
                    if (n <= 0 || buf == 0)
                    {
                        // 排空剩余消息
                        uint32_t len = 0;
                        uint32_t seq = 0;
                        while (csp->channel.TryRead(csp->read_buf.get(), &len, &seq) == 0)
                        {
                            std::printf("server: client #%d [seq=%03u len=%u]: %.*s\n",
                                        csp->id, seq, len,
                                        static_cast<int>(len), csp->read_buf.get());
                            ++csp->total_read;
                        }
                        std::printf("server: client #%d disconnected, total read=%d\n",
                                    csp->id, csp->total_read);
                        RemoveClient(loop, clients, client_fd, csp);
                        return;
                    }
                }
            });

            clients[client_fd] = std::move(cs);
        });

        loop.Run();

        std::printf("\nserver: shutting down (%zu clients remaining)\n", clients.size());
        clients.clear();
        ::unlink(shm_ipc::kDefaultSocketPath);

    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "server: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
