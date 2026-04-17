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
#include <shm_ipc/frame_reader.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/pod_codec.hpp>

#ifdef SHM_IPC_HAS_PROTOBUF
#include <shm_ipc/proto_codec.hpp>
#include "shm_messages.pb.h"
#endif

namespace {

constexpr int kTimerIntervalMs = 100;  ///< 定时器间隔（毫秒）
constexpr int kMaxTicks        = 100;  ///< 最大 tick 数

using Channel = shm::DefaultRingChannel;

/// 全局事件循环指针，供信号处理器使用
shm::EventLoop *gLoop = nullptr;

/// 是否使用 protobuf 编解码
bool gUseProto = false;

/// 全局 PodCodec 实例
shm::PodCodec<ClientMsg>  gClientMsgCodec;
shm::PodCodec<Heartbeat>  gHeartbeatCodec;

#ifdef SHM_IPC_HAS_PROTOBUF
/// 全局 ProtoCodec 实例
shm::ProtoCodec<shm_ipc::ClientMsgProto>  gProtoClientMsgCodec;
shm::ProtoCodec<shm_ipc::HeartbeatProto>  gProtoHeartbeatCodec;
#endif

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
shm::UniqueFd ConnectToServer(const char *path)
{
    shm::UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
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
void OnWriteTimer(int tfd, shm::ClientState &cs, shm::EventLoop &loop,
                  WriteStats &stats, std::mt19937 &rng,
                  std::uniform_int_distribution<uint32_t> &len_dist)
{
    shm::EventLoop::DrainTimerfd(tfd);

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

    int send_rc = -1;
#ifdef SHM_IPC_HAS_PROTOBUF
    if (gUseProto)
    {
        shm_ipc::ClientMsgProto cm;
        cm.set_seq(cs.tick);
        cm.set_tick(cs.tick);
        cm.set_timestamp(std::time(nullptr));
        cm.set_payload(std::string(msg.payload_len, 'C'));
        send_rc = gProtoClientMsgCodec.Send(cs.channel, cm, static_cast<uint32_t>(cs.tick));
    }
    else
#endif
    {
        send_rc = gClientMsgCodec.Send(cs.channel, msg, static_cast<uint32_t>(cs.tick));
    }

    if (send_rc == 0)
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
void OnServerNotify(int efd, shm::ClientState &cs,
                    shm::CodecReader<> &reader)
{
    Channel::DrainNotify(efd);

#ifdef SHM_IPC_HAS_PROTOBUF
    if (gUseProto)
    {
        const void *data = nullptr;
        uint32_t data_len = 0;
        while (reader.TryRecv(cs.channel, &data, &data_len) == 0)
        {
            shm_ipc::HeartbeatProto hb;
            if (hb.ParseFromArray(data, static_cast<int>(data_len)))
            {
                std::printf("client: heartbeat [client_id=%d seq=%d ts=%ld] (proto)\n",
                            hb.client_id(), hb.seq(),
                            static_cast<long>(hb.timestamp()));
            }
            ++cs.total_read;
        }
        return;
    }
#endif

    const void *data = nullptr;
    uint32_t data_len = 0;
    while (reader.TryRecv(cs.channel, &data, &data_len) == 0)
    {
        Heartbeat hb{};
        if (data_len == sizeof(Heartbeat))
            std::memcpy(&hb, data, sizeof(Heartbeat));
        std::printf("client: heartbeat [client_id=%d seq=%d ts=%ld]\n",
                    hb.client_id, hb.seq, static_cast<long>(hb.timestamp));
        ++cs.total_read;
    }
}

/**
 * @brief Socket 回调：仅用于断连检测
 */
void OnServerSocket(int fd, short revents, shm::ClientState & /*cs*/,
                    shm::EventLoop &loop)
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
    auto sfd = ConnectToServer(shm::kDefaultSocketPath);
    std::printf("client(bench): connected\n");

    auto ch = Channel::Connect(sfd.Get());
    std::printf("client(bench): ring established\n\n");

    shm::PrintBenchHeader();

    for (auto &tc : shm::kBenchCases)
    {
        // 通知 server 本轮参数
        shm::BenchCmd cmd{tc.payload_size, tc.rounds};
        (void)::write(sfd.Get(), &cmd, sizeof(cmd));

        // 构造 payload = [tag u32] + BenchPayloadHeader + 填充字节
        uint32_t body_len = shm::kTagSize
                          + static_cast<uint32_t>(sizeof(shm::BenchPayloadHeader))
                          + tc.payload_size;
        std::vector<char> body(body_len);
        uint32_t bench_tag = shm::kBenchTag;
        std::memcpy(body.data(), &bench_tag, shm::kTagSize);
        std::memset(body.data() + shm::kTagSize + sizeof(shm::BenchPayloadHeader),
                    'B', tc.payload_size);

        // 忙循环批量发送
        uint64_t t0 = shm::NowNs();
        for (int32_t i = 0; i < tc.rounds; )
        {
            auto batch = ch.StartBatch();
            while (i < tc.rounds)
            {
                shm::BenchPayloadHeader hdr{i};
                std::memcpy(body.data() + shm::kTagSize, &hdr, sizeof(hdr));
                if (shm::Send<Channel::Ring::capacity>(
                        batch, body.data(), body_len,
                        static_cast<uint32_t>(i)) != 0)
                    break;  // ring 满，Flush 这批后重新开始
                ++i;
            }
            batch.Flush();
        }

        // 发结束标记
        {
            char end_body[shm::kTagSize + sizeof(shm::BenchPayloadHeader)];
            std::memcpy(end_body, &bench_tag, shm::kTagSize);
            shm::BenchPayloadHeader end_hdr{-1};
            std::memcpy(end_body + shm::kTagSize, &end_hdr, sizeof(end_hdr));
            while (shm::Send(ch, end_body, sizeof(end_body), 0) != 0)
                ;
        }

        // 等 server ack
        char ack = 0;
        (void)::read(sfd.Get(), &ack, 1);
        uint64_t elapsed = shm::NowNs() - t0;

        shm::PrintBenchRow(tc.payload_size, tc.rounds, elapsed);
    }

    // 通知 server 结束
    shm::BenchCmd end{0, 0};
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

        shm::ClientState cs;
        cs.socket_fd = ConnectToServer(shm::kDefaultSocketPath);
        std::printf("client: connected to %s\n", shm::kDefaultSocketPath);

        cs.channel    = Channel::Connect(cs.socket_fd.Get());
        cs.notify_efd = cs.channel.NotifyReadFd();
        std::printf("client: ring channel established (capacity=%zuMB, "
                    "eventfd notify)\n\n",
                    Channel::Ring::capacity / (1024 * 1024));

        shm::EventLoop loop;
        gLoop = &loop;

        WriteStats stats;
#ifdef SHM_IPC_HAS_PROTOBUF
        shm::CodecReader<> hb_reader{gUseProto
            ? static_cast<shm::ICodec *>(&gProtoHeartbeatCodec)
            : static_cast<shm::ICodec *>(&gHeartbeatCodec)};
#else
        shm::CodecReader<> hb_reader{&gHeartbeatCodec};
#endif

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

        std::printf("client: timer=%dms, max_ticks=%d, codec=%s\n\n",
                    kTimerIntervalMs, kMaxTicks,
                    gUseProto ? "proto" : "pod");
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
