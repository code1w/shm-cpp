/**
 * @file app_server.cpp
 * @brief shm::ShmServer 上层接口示例：多客户端 echo 服务
 *
 * 应用代码只依赖 comm:: 接口类型，传输实现可随时替换为 io::TcpServer。
 */

#include <csignal>
#include <cstdio>
#include <ctime>
#include <memory>

#include <shm_ipc/messages.hpp>
#include <shm_ipc/shm_server.hpp>
#include <shm_ipc/simple_pod_codec.hpp>

namespace {

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

    // 注册 POD 消息类型
    shm::codec::SimplePodCodec::RegisterPod<Heartbeat>();
    shm::codec::SimplePodCodec::RegisterPod<ClientMsg>();

    shm::EventLoop loop;
    gLoop = &loop;

    shm::ShmServer server(&loop, shm::kDefaultSocketPath, "app_server");

    // 每条连接一个 codec 实例（与框架用法一致）
    server.SetCodecFactory([] {
        return std::make_shared<shm::codec::SimplePodCodec>();
    });
    server.SetConnectionCallback([](const comm::TcpConnPtr &conn) {
        std::printf("server: conn #%lu established, remote: %s\n",
                    static_cast<unsigned long>(conn->Id()),
                    conn->RemoteAddr().c_str());
    });
    server.SetCloseCallback([](const comm::TcpConnPtr &conn) {
        std::printf("server: conn #%lu closed (type=%d)\n",
                    static_cast<unsigned long>(conn->Id()),
                    static_cast<int>(conn->GetCloseType()));
    });
    // 消息回调：std::any 模型，与框架 AnyMessageCallback 一致
    server.SetMessageCallback([](const comm::TcpConnPtr &conn,
                                 const std::any &pkg) {
        if (const auto *msg = std::any_cast<ClientMsg>(&pkg))
        {
            std::printf("server: conn #%lu [tick=%d payload_len=%u]\n",
                        static_cast<unsigned long>(conn->Id()), msg->tick,
                        msg->payload_len);
            Heartbeat hb{};
            hb.client_id = static_cast<int32_t>(conn->Id());
            hb.seq       = msg->seq;
            hb.timestamp = std::time(nullptr);
            conn->SendCmd(hb);
        }
    });
    server.SetStartFailedCallback([](std::string err) {
        std::fprintf(stderr, "server: %s\n", err.c_str());
    });

    if (!server.Start())
        return 1;

    std::printf("server: listening on %s (max 16 clients)\n",
                shm::kDefaultSocketPath);
    loop.Run();

    std::printf("server: bye\n");
    return 0;
}
