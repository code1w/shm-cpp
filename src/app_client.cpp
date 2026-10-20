/**
 * @file app_client.cpp
 * @brief shm::ShmAsyncClient 上层接口示例：定时发送 ClientMsg、接收 Heartbeat
 *
 * 应用代码只依赖 comm:: 接口类型，传输实现可随时替换为 io::TcpAsyncClient。
 */

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#include <shm_ipc/messages.hpp>
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/simple_pod_codec.hpp>

namespace {

constexpr int kTimerIntervalMs = 100;  ///< 发送定时间隔（毫秒）
constexpr int kMaxTicks        = 50;   ///< 最大 tick 数

/// 全局事件循环指针，供信号处理器使用
shm::EventLoop *gLoop = nullptr;

void SignalHandler(int /*signo*/)
{
    if (gLoop)
        gLoop->Stop();
}

}  // anonymous namespace

int main()
{
    // 向已断开的 socket 写入会触发 SIGPIPE 杀死进程，忽略之
    ::signal(SIGPIPE, SIG_IGN);

    struct sigaction sa
    {};
    sa.sa_handler = SignalHandler;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    shm::codec::SimplePodCodec::RegisterPod<Heartbeat>();
    shm::codec::SimplePodCodec::RegisterPod<ClientMsg>();

    shm::EventLoop loop;
    gLoop = &loop;

    shm::ShmAsyncClient client(&loop);

    client.SetCodecFactory([] {
        return std::make_shared<shm::codec::SimplePodCodec>();
    });
    client.SetConnectionCallback([](const comm::TcpConnPtr &conn) {
        std::printf("client: connected (conn #%lu)\n",
                    static_cast<unsigned long>(conn->Id()));
    });
    client.SetCloseCallback([](const comm::TcpConnPtr &) {
        std::printf("client: connection closed\n");
        if (gLoop)
            gLoop->Stop();
    });
    client.SetConnectErrorCallback([](bool /*autoreconnect*/,
                                      const std::string &uri) {
        std::fprintf(stderr, "client: connect failed: %s\n", uri.c_str());
        if (gLoop)
            gLoop->Stop();
    });
    // 消息回调：std::any 模型，与框架 AnyMessageCallback 一致
    client.SetMessageCallback([](const comm::TcpConnPtr &, const std::any &pkg) {
        if (const auto *hb = std::any_cast<Heartbeat>(&pkg))
        {
            std::printf("client: heartbeat [client_id=%d seq=%d ts=%ld]\n",
                        hb->client_id, hb->seq,
                        static_cast<long>(hb->timestamp));
        }
    });

    client.Connect(shm::kDefaultSocketPath);
    if (client.State() != comm::TcpClientStates::established)
        return 1;

    // 定时发送 ClientMsg
    int tick = 0;
    loop.AddTimer(kTimerIntervalMs, [&](int tfd, short) {
        shm::EventLoop::DrainTimerfd(tfd);

        if (++tick > kMaxTicks)
        {
            client.Close();
            loop.Stop();
            return;
        }

        ClientMsg msg{};
        msg.seq         = tick;
        msg.tick        = tick;
        msg.timestamp   = std::time(nullptr);
        msg.payload_len = 64;
        std::memset(msg.payload, 'C', msg.payload_len);

        if (!client.SendCmd(msg))
            std::fprintf(stderr, "client: [tick=%d] send failed\n", tick);
    });

    std::printf("client: timer=%dms, max_ticks=%d\n", kTimerIntervalMs,
                kMaxTicks);
    loop.Run();

    std::printf("client: bye\n");
    return 0;
}
