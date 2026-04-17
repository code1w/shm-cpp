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

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <unordered_map>

#include <shm_ipc/bench_common.hpp>
#include <shm_ipc/client_state.hpp>
#include <shm_ipc/codec.hpp>
#include <shm_ipc/event_loop.hpp>
#include <shm_ipc/frame_reader.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/pod_codec.hpp>

#ifdef SHM_IPC_HAS_PROTOBUF
#include <shm_ipc/proto_codec.hpp>
#include "shm_messages.pb.h"
#endif

namespace {

constexpr int kHeartbeatMs = 300;  ///< 心跳定时器间隔（毫秒）
constexpr int kMaxTicks    = 40;   ///< 每客户端最大心跳次数
constexpr int kMaxClients  = 16;   ///< 最大并发客户端数

using Channel     = shm::DefaultRingChannel;
using ClientState = shm::ClientState;
using Clients     = std::unordered_map<int, std::unique_ptr<ClientState>>;

/// 全局 PodCodec 实例（写方向）
shm::PodCodec<Heartbeat>  gHeartbeatCodec;

#ifdef SHM_IPC_HAS_PROTOBUF
/// 全局 ProtoCodec 实例（写方向）
shm::ProtoCodec<shm_ipc::HeartbeatProto>  gProtoHeartbeatCodec;
#endif

/// 全局事件循环指针，供信号处理器使用
shm::EventLoop *gLoop = nullptr;

/// 是否使用 protobuf 编解码
bool gUseProto = false;

void SignalHandler(int /*signo*/)
{
    if (gLoop)
        gLoop->Stop();
}

// ---------------------------------------------------------------------------
// 打印重载
// ---------------------------------------------------------------------------

void PrintMsg(int client_id, const ClientMsg &m)
{
    std::printf("server: client #%d [tick=%d payload_len=%u]\n",
                client_id, m.tick, m.payload_len);
}

#ifdef SHM_IPC_HAS_PROTOBUF
void PrintMsg(int client_id, const shm_ipc::ClientMsgProto &m)
{
    std::printf("server: client #%d [tick=%d payload_len=%zu] (proto)\n",
                client_id, m.tick(), m.payload().size());
}
#endif

// ---------------------------------------------------------------------------
// 辅助函数
// ---------------------------------------------------------------------------

/**
 * @brief 创建并绑定 Unix domain socket 监听端
 */
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
    if (::listen(sfd.Get(), 5) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));

    return sfd;
}

/**
 * @brief 从事件循环中移除客户端的所有关联 fd 并销毁状态
 */
void RemoveClient(shm::EventLoop &loop, Clients &clients,
                  int client_fd, ClientState *csp)
{
    loop.RemoveTimer(csp->timer_fd);
    loop.RemoveFd(csp->notify_efd);
    loop.RemoveFd(client_fd);
    clients.erase(client_fd);
}

/**
 * @brief 从客户端环形缓冲区中读取并反序列化所有待处理消息
 */
int DrainClientRing(ClientState *csp)
{
    int count = 0;
#ifdef SHM_IPC_HAS_PROTOBUF
    if (gUseProto)
    {
        shm_ipc::ClientMsgProto msg{};
        while (csp->codec->Recv(csp->channel, &msg) == 0)
        {
            PrintMsg(csp->id, msg);
            ++csp->total_read;
            ++count;
            msg = shm_ipc::ClientMsgProto{};
        }
        return count;
    }
#endif
    ClientMsg msg{};
    while (csp->codec->Recv(csp->channel, &msg) == 0)
    {
        PrintMsg(csp->id, msg);
        ++csp->total_read;
        ++count;
        msg = ClientMsg{};
    }
    return count;
}

// ---------------------------------------------------------------------------
// EventLoop 回调函数
// ---------------------------------------------------------------------------

/**
 * @brief 心跳定时器回调：向客户端发送 Heartbeat POD
 */
void OnHeartbeatTimer(int timer_fd, shm::EventLoop &loop,
                      Clients &clients, int client_fd)
{
    shm::EventLoop::DrainTimerfd(timer_fd);

    auto it = clients.find(client_fd);
    if (it == clients.end())
        return;
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

#ifdef SHM_IPC_HAS_PROTOBUF
    if (gUseProto)
    {
        shm_ipc::HeartbeatProto pb_hb;
        pb_hb.set_client_id(csp->id);
        pb_hb.set_seq(csp->tick);
        pb_hb.set_timestamp(std::time(nullptr));
        if (gProtoHeartbeatCodec.Send(csp->channel, pb_hb,
                                      static_cast<uint32_t>(csp->tick)) != 0)
        {
            std::fprintf(stderr, "server: client #%d heartbeat write full (proto)\n",
                         csp->id);
        }
        return;
    }
#endif

    if (gHeartbeatCodec.Send(csp->channel, hb,
                             static_cast<uint32_t>(csp->tick)) != 0)
    {
        std::fprintf(stderr, "server: client #%d heartbeat write full\n",
                     csp->id);
    }
}

/**
 * @brief Eventfd 回调：客户端写入新数据，立即实时读取
 */
void OnClientNotify(int efd, Clients &clients, int client_fd)
{
    Channel::DrainNotify(efd);
    auto it = clients.find(client_fd);
    if (it == clients.end())
        return;
    ClientState *csp = it->second.get();
    int count = DrainClientRing(csp);
    if (count > 0)
    {
        std::printf("server: client #%d realtime read count=%d total=%d\n",
                    csp->id, count, csp->total_read);
    }
}

/**
 * @brief Socket 回调：仅用于断连检测
 */
void OnClientSocket(int fd, short revents, shm::EventLoop &loop,
                    Clients &clients, int client_fd)
{
    auto it = clients.find(client_fd);
    if (it == clients.end())
        return;
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
        auto n = ::read(fd, &buf, 1);
        if (n <= 0 || buf == 0)
        {
            int remaining = DrainClientRing(csp);
            std::printf("server: client #%d disconnected, "
                        "drained %d remaining, total read=%d\n",
                        csp->id, remaining, csp->total_read);
            RemoveClient(loop, clients, client_fd, csp);
            return;
        }
    }
}

/**
 * @brief Accept 回调：接受新客户端连接并注册所有回调
 */
void OnAccept(int lfd, shm::EventLoop &loop,
              Clients &clients, int &next_client_id)
{
    if (static_cast<int>(clients.size()) >= kMaxClients)
    {
        std::fprintf(stderr, "server: max clients reached, rejecting\n");
        shm::UniqueFd rejected{::accept(lfd, nullptr, nullptr)};
        return;
    }

    shm::UniqueFd cfd{::accept(lfd, nullptr, nullptr)};
    if (!cfd)
        return;

    int cid = next_client_id++;
    std::printf("server: client #%d connected (fd=%d)\n", cid, cfd.Get());

    auto cs      = std::make_unique<ClientState>();
    cs->id       = cid;
    cs->channel  = Channel::Accept(cfd.Get());
    std::printf("server: client #%d ring channel established\n", cid);

#ifdef SHM_IPC_HAS_PROTOBUF
    if (gUseProto)
        cs->codec = std::make_unique<shm::ProtoCodec<shm_ipc::ClientMsgProto>>();
    else
#endif
        cs->codec = std::make_unique<shm::PodCodec<ClientMsg>>();

    int client_fd  = cfd.Get();
    cs->socket_fd  = std::move(cfd);
    cs->notify_efd = cs->channel.NotifyReadFd();

    // 注册心跳定时器
    int tfd = loop.AddTimer(kHeartbeatMs,
        std::bind(OnHeartbeatTimer, std::placeholders::_1,
                  std::ref(loop), std::ref(clients), client_fd));
    cs->timer_fd = tfd;

    // 注册 eventfd 实时读
    loop.AddFd(cs->notify_efd,
        std::bind(OnClientNotify, std::placeholders::_1,
                  std::ref(clients), client_fd));

    // 注册 socket 断连检测
    loop.AddFd(client_fd,
        std::bind(OnClientSocket, std::placeholders::_1, std::placeholders::_2,
                  std::ref(loop), std::ref(clients), client_fd));

    clients[client_fd] = std::move(cs);
}

// ---------------------------------------------------------------------------
// Bench 模式
// ---------------------------------------------------------------------------

void RunBench()
{
    auto listen_fd = CreateListenSocket(shm::kDefaultSocketPath);
    std::printf("server(bench): waiting for client on %s ...\n",
                shm::kDefaultSocketPath);

    shm::UniqueFd cfd{::accept(listen_fd.Get(), nullptr, nullptr)};
    if (!cfd)
    {
        std::perror("accept");
        return;
    }

    auto ch = Channel::Accept(cfd.Get());
    std::printf("server(bench): client connected, ring established\n\n");

    shm::FrameReader<3 * 1024 * 1024> reader;
    shm::PrintBenchHeader();

    for (;;)
    {
        shm::BenchCmd cmd{};
        ssize_t n = ::read(cfd.Get(), &cmd, sizeof(cmd));
        if (n != sizeof(cmd) || cmd.rounds == 0)
            break;

        int32_t received = 0;
        const void *payload = nullptr;
        uint32_t payload_len = 0;

        uint64_t t0 = shm::NowNs();
        bool got_end = false;
        while (!got_end)
        {
            if (reader.TryRecv(ch, &payload, &payload_len) == 0)
            {
                // payload = [tag u32][BenchPayloadHeader][fill]
                // 跳过 tag 后检查 BenchPayloadHeader
                if (payload_len >= shm::kTagSize + sizeof(shm::BenchPayloadHeader))
                {
                    shm::BenchPayloadHeader ph{};
                    std::memcpy(&ph,
                                static_cast<const char *>(payload) + shm::kTagSize,
                                sizeof(ph));
                    if (ph.seq == -1)
                    {
                        got_end = true;
                        break;
                    }
                }
                ++received;
            }
        }
        reader.Commit(ch);
        uint64_t elapsed = shm::NowNs() - t0;

        // 发 ack
        char ack = 1;
        (void)::write(cfd.Get(), &ack, 1);

        shm::PrintBenchRow(cmd.payload_size, received, elapsed);
    }

    std::printf("\nserver(bench): done\n");
    ::unlink(shm::kDefaultSocketPath);
}

}  // anonymous namespace

int main(int argc, char *argv[])
{
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--bench") == 0)
            {
                RunBench();
                return 0;
            }
            if (std::strcmp(argv[i], "--cmd") == 0 && i + 1 < argc)
            {
                if (std::strcmp(argv[i + 1], "proto") == 0)
                    gUseProto = true;
                ++i;
            }
        }

        struct sigaction sa
        {};
        sa.sa_handler = SignalHandler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        shm::EventLoop loop;
        gLoop = &loop;

        Clients clients;
        int next_client_id = 1;

        auto listen_fd = CreateListenSocket(shm::kDefaultSocketPath);
        std::printf("server: listening on %s (max %d clients, ring=%zuMB, realtime "
                    "eventfd read, codec=%s)\n",
                    shm::kDefaultSocketPath, kMaxClients,
                    Channel::Ring::capacity / (1024 * 1024),
                    gUseProto ? "proto" : "pod");

        loop.AddFd(listen_fd.Get(),
            std::bind(OnAccept, std::placeholders::_1,
                      std::ref(loop), std::ref(clients), std::ref(next_client_id)));

        loop.Run();

        std::printf("\nserver: shutting down (%zu clients remaining)\n",
                    clients.size());
        clients.clear();
        ::unlink(shm::kDefaultSocketPath);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "server: error: %s\n", e.what());
        return EXIT_FAILURE;
    }
    return 0;
}
