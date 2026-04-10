/**
 * @file server.cpp
 * @brief 多客户端服务端，基于 RingChannel + EventLoop（实时读版本）
 *
 * 读取策略：客户端 eventfd 触发时立即消费所有消息（实时），
 * 不再依赖定时器批量读。定时器仅用于向客户端发送心跳。
 *
 * 通知机制：使用握手时交换的共享 eventfd。
 * Unix socket 仅用于 fd 交换和断连检测。
 */

#include <shm_ipc/client_state.hpp>
#include <shm_ipc/codec.hpp>
#include <shm_ipc/event_loop.hpp>
#include <shm_ipc/messages.hpp>

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

constexpr int kHeartbeatMs = 300;  ///< 心跳定时器间隔（毫秒）
constexpr int kMaxTicks    = 40;   ///< 每客户端最大心跳次数
constexpr int kMaxClients  = 16;   ///< 最大并发客户端数

using Channel     = shm_ipc::DefaultRingChannel;
using ClientState = shm_ipc::ClientState;

/// 每客户端的流式 POD 读取器
std::unordered_map<int, shm_ipc::PodReader<ClientMsg>> gReaders;

/// 全局事件循环指针，供信号处理器使用
shm_ipc::EventLoop *gLoop = nullptr;

void SignalHandler(int /*signo*/)
{
    if (gLoop)
        gLoop->Stop();
}

/**
 * @brief 创建并绑定 Unix domain socket 监听端
 * @param path socket 文件路径
 * @return 已处于 listen 状态的 UniqueFd
 */
shm_ipc::UniqueFd CreateListenSocket(const char *path)
{
    shm_ipc::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    ::unlink(path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::bind(sfd.Get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(sfd.Get(), 5) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));

    return sfd;
}

/**
 * @brief 从事件循环中移除客户端的所有关联 fd 并销毁状态
 * @param loop      事件循环
 * @param clients   客户端状态映射表
 * @param client_fd 客户端 socket fd（作为 map key）
 * @param csp       客户端状态指针
 */
void RemoveClient(
    shm_ipc::EventLoop &loop,
    std::unordered_map<int, std::unique_ptr<ClientState>> &clients,
    int client_fd, ClientState *csp)
{
    loop.RemoveTimer(csp->timer_fd);
    loop.RemoveFd(csp->notify_efd);
    loop.RemoveFd(client_fd);
    gReaders.erase(client_fd);
    clients.erase(client_fd);
}

/**
 * @brief 从客户端环形缓冲区中流式拆包所有待处理的 ClientMsg
 * @param client_fd 客户端 fd（用于索引 PodReader）
 * @param csp       客户端状态指针
 * @return 本次读取的消息条数
 */
int DrainClientRing(int client_fd, ClientState *csp)
{
    auto &reader = gReaders[client_fd];
    ClientMsg msg{};
    int count = 0;
    while (reader.TryRecv(csp->channel, &msg) == 0)
    {
        std::printf("server: client #%d [tick=%d payload_len=%u]\n", csp->id,
                    msg.tick, msg.payload_len);
        ++csp->total_read;
        ++count;
    }
    return count;
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

        shm_ipc::EventLoop loop;
        gLoop = &loop;

        std::unordered_map<int, std::unique_ptr<ClientState>> clients;
        int next_client_id = 1;

        auto listen_fd = CreateListenSocket(shm_ipc::kDefaultSocketPath);
        std::printf("server: listening on %s (max %d clients, ring=%zuMB, realtime "
                    "eventfd read)\n",
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
            if (!cfd)
                return;

            int cid = next_client_id++;
            std::printf("server: client #%d connected (fd=%d)\n", cid, cfd.Get());

            auto cs      = std::make_unique<ClientState>();
            cs->id       = cid;
            cs->channel  = Channel::Accept(cfd.Get());
            cs->read_buf = std::make_unique<char[]>(Channel::max_msg_size);
            std::printf("server: client #%d ring channel established\n", cid);

            int client_fd  = cfd.Get();
            cs->socket_fd  = std::move(cfd);
            cs->notify_efd = cs->channel.NotifyReadFd();

            ClientState *csp = cs.get();

            // --- 心跳定时器：向客户端发送 Heartbeat POD ---
            int tfd       = loop.AddTimer(kHeartbeatMs, [&loop, &clients, client_fd](int timer_fd, short /*rev*/) {
                shm_ipc::EventLoop::DrainTimerfd(timer_fd);

                auto it = clients.find(client_fd);
                if (it == clients.end())
                    return;  // 已被其他回调移除
                ClientState *csp = it->second.get();

                if (++csp->tick > kMaxTicks)
                {
                    std::printf("server: client #%d max ticks reached, disconnecting\n",
                                      csp->id);
                    RemoveClient(loop, clients, client_fd, csp);
                    return;
                }

                Heartbeat hb{};
                hb.client_id = csp->id;
                hb.seq       = csp->tick;
                hb.timestamp = std::time(nullptr);

                if (shm_ipc::SendPod(csp->channel, hb,
                                           static_cast<uint32_t>(csp->tick)) != 0)
                {
                    std::fprintf(stderr, "server: client #%d heartbeat write full\n",
                                       csp->id);
                }
            });
            csp->timer_fd = tfd;

            // --- Eventfd：客户端写入新数据，立即实时读取 ---
            loop.AddFd(csp->notify_efd, [&clients, client_fd](int efd, short /*revents*/) {
                Channel::DrainNotify(efd);
                auto it = clients.find(client_fd);
                if (it == clients.end())
                    return;  // 已被其他回调移除
                ClientState *csp = it->second.get();
                int count = DrainClientRing(client_fd, csp);
                if (count > 0)
                {
                    std::printf("server: client #%d realtime read count=%d total=%d\n",
                                csp->id, count, csp->total_read);
                }
            });

            // --- Socket：仅用于断连检测 ---
            loop.AddFd(client_fd, [&loop, &clients, client_fd](int /*fd*/,
                                                                    short revents) {
                auto it = clients.find(client_fd);
                if (it == clients.end())
                    return;  // 已被其他回调移除
                ClientState *csp = it->second.get();

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
                        // 排空断连前残留的消息
                        int remaining = DrainClientRing(client_fd, csp);
                        std::printf("server: client #%d disconnected, "
                                    "drained %d remaining, total read=%d\n",
                                    csp->id, remaining, csp->total_read);
                        RemoveClient(loop, clients, client_fd, csp);
                        return;
                    }
                }
            });

            clients[client_fd] = std::move(cs);
        });

        loop.Run();

        std::printf("\nserver: shutting down (%zu clients remaining)\n",
                    clients.size());
        clients.clear();
        ::unlink(shm_ipc::kDefaultSocketPath);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "server: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
