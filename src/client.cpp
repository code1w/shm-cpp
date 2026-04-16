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
#include <functional>
#include <memory>
#include <random>
#include <vector>

#include <shm_ipc/bench_common.hpp>
#include <shm_ipc/client_state.hpp>
#include <shm_ipc/codec.hpp>
#include <shm_ipc/event_loop.hpp>
#include <shm_ipc/messages.hpp>

namespace {

constexpr int kTimerIntervalMs = 100;  ///< 定时器间隔（毫秒）
constexpr int kMaxTicks        = 100;  ///< 最大 tick 数

using Channel = shm_ipc::DefaultRingChannel;

/// 全局事件循环指针，供信号处理器使用
shm_ipc::EventLoop *gLoop = nullptr;

void SignalHandler(int /*signo*/)
{
    if (gLoop)
        gLoop->Stop();
}

/// 写入统计
struct WriteStats
{
    int ok   = 0;
    int full = 0;
};

// ---------------------------------------------------------------------------
// 辅助函数
// ---------------------------------------------------------------------------

/**
 * @brief 连接到服务端的 Unix domain socket
 */
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

// ---------------------------------------------------------------------------
// EventLoop 回调函数
// ---------------------------------------------------------------------------

/**
 * @brief 定时器回调：向环形缓冲区写 ClientMsg POD（随机 payload 长度）
 */
void OnWriteTimer(int tfd, shm_ipc::ClientState &cs, shm_ipc::EventLoop &loop,
                  WriteStats &stats, std::mt19937 &rng,
                  std::uniform_int_distribution<uint32_t> &len_dist)
{
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

    if (shm_ipc::SendPod(cs.channel, msg, static_cast<uint32_t>(cs.tick)) == 0)
    {
        ++stats.ok;
    }
    else
    {
        ++stats.full;
        std::fprintf(stderr, "client: [seq=%03d] ring full! (ok=%d, full=%d)\n",
                     cs.tick, stats.ok, stats.full);
    }

    if (cs.tick % 10 == 0)
    {
        std::printf("client: --- tick=%d ok=%d full=%d ring_bytes=%lu ---\n",
                    cs.tick, stats.ok, stats.full,
                    static_cast<unsigned long>(cs.channel.Readable()));
    }
}

/**
 * @brief Eventfd 回调：服务端向读环写入新数据，流式拆包 Heartbeat
 */
void OnServerNotify(int efd, shm_ipc::ClientState &cs,
                    shm_ipc::FrameReader<> &reader)
{
    Channel::DrainNotify(efd);
    uint32_t tag = 0;
    const void *payload = nullptr;
    uint32_t payload_len = 0;
    while (reader.TryRecv(cs.channel, &tag, &payload, &payload_len) == 0)
    {
        Heartbeat hb{};
        if (shm_ipc::DecodePod<Heartbeat>(payload, payload_len, &hb))
        {
            std::printf("client: heartbeat [client_id=%d seq=%d ts=%ld]\n",
                        hb.client_id, hb.seq, static_cast<long>(hb.timestamp));
        }
        ++cs.total_read;
    }
}

/**
 * @brief Socket 回调：仅用于断连检测
 */
void OnServerSocket(int fd, short revents, shm_ipc::ClientState & /*cs*/,
                    shm_ipc::EventLoop &loop)
{
    if (revents & (POLLHUP | POLLERR))
    {
        std::printf("client: server disconnected\n");
        loop.Stop();
        return;
    }
    if (revents & POLLIN)
    {
        char buf{};
        auto n = ::read(fd, &buf, 1);
        if (n <= 0 || buf == 0)
        {
            std::printf("client: server disconnected\n");
            loop.Stop();
        }
    }
}

// ---------------------------------------------------------------------------
// Bench 模式
// ---------------------------------------------------------------------------

void RunBench()
{
    auto sfd = ConnectToServer(shm_ipc::kDefaultSocketPath);
    std::printf("client(bench): connected\n");

    auto ch = Channel::Connect(sfd.Get());
    std::printf("client(bench): ring established\n\n");

    shm_ipc::PrintBenchHeader();

    for (auto &tc : shm_ipc::kBenchCases)
    {
        // 通知 server 本轮参数
        shm_ipc::BenchCmd cmd{tc.payload_size, tc.rounds};
        (void)::write(sfd.Get(), &cmd, sizeof(cmd));

        // 构造 payload = BenchPayloadHeader + 填充字节
        uint32_t body_len = static_cast<uint32_t>(sizeof(shm_ipc::BenchPayloadHeader))
                          + tc.payload_size;
        std::vector<char> body(body_len);
        std::memset(body.data() + sizeof(shm_ipc::BenchPayloadHeader), 'B',
                    tc.payload_size);

        // 忙循环发送
        uint64_t t0 = shm_ipc::NowNs();
        for (int32_t i = 0; i < tc.rounds; ++i)
        {
            shm_ipc::BenchPayloadHeader hdr{i};
            std::memcpy(body.data(), &hdr, sizeof(hdr));
            while (shm_ipc::Send(ch, shm_ipc::kBenchTag,
                                 body.data(), body_len,
                                 static_cast<uint32_t>(i)) != 0)
                ;  // spin-wait ring full
        }

        // 发结束标记（仅 header，无填充）
        shm_ipc::BenchPayloadHeader end_hdr{-1};
        while (shm_ipc::Send(ch, shm_ipc::kBenchTag,
                             &end_hdr, sizeof(end_hdr), 0) != 0)
            ;

        // 等 server ack
        char ack = 0;
        (void)::read(sfd.Get(), &ack, 1);
        uint64_t elapsed = shm_ipc::NowNs() - t0;

        shm_ipc::PrintBenchRow(tc.payload_size, tc.rounds, elapsed);
    }

    // 通知 server 结束
    shm_ipc::BenchCmd end{0, 0};
    (void)::write(sfd.Get(), &end, sizeof(end));

    std::printf("\nclient(bench): done\n");
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
        }

        struct sigaction sa
        {};
        sa.sa_handler = SignalHandler;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);

        shm_ipc::ClientState cs;
        cs.socket_fd = ConnectToServer(shm_ipc::kDefaultSocketPath);
        std::printf("client: connected to %s\n", shm_ipc::kDefaultSocketPath);

        cs.channel    = Channel::Connect(cs.socket_fd.Get());
        cs.notify_efd = cs.channel.NotifyReadFd();
        std::printf("client: ring channel established (capacity=%zuMB, "
                    "eventfd notify)\n\n",
                    Channel::Ring::capacity / (1024 * 1024));

        shm_ipc::EventLoop loop;
        gLoop = &loop;

        WriteStats stats;
        shm_ipc::FrameReader<> hb_reader;

        std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<uint32_t> len_dist(
            16, sizeof(ClientMsg::payload));

        // 注册定时器
        loop.AddTimer(kTimerIntervalMs,
            std::bind(OnWriteTimer, std::placeholders::_1,
                      std::ref(cs), std::ref(loop), std::ref(stats),
                      std::ref(rng), std::ref(len_dist)));

        // 注册 eventfd 读取心跳
        loop.AddFd(cs.notify_efd,
            std::bind(OnServerNotify, std::placeholders::_1,
                      std::ref(cs), std::ref(hb_reader)));

        // 注册 socket 断连检测
        loop.AddFd(cs.socket_fd.Get(),
            std::bind(OnServerSocket, std::placeholders::_1, std::placeholders::_2,
                      std::ref(cs), std::ref(loop)));

        std::printf("client: timer=%dms, max_ticks=%d\n\n", kTimerIntervalMs,
                    kMaxTicks);
        loop.Run();

        std::printf("\nclient: done. ok=%d full=%d total_read=%d\n", stats.ok,
                    stats.full, cs.total_read);
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
