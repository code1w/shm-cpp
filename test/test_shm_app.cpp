/**
 * @file test_shm_app.cpp
 * @brief 上层应用接口集成测试：ShmServer / ShmAsyncClient / ShmClient
 *
 * 单进程内完成：
 * 1. ShmServer 事件循环跑在独立线程，对 Heartbeat 做 echo（std::any 模型）；
 * 2. 主线程用同步 ShmClient 做 5 轮请求/响应（codec 手动编解码原始字节流）；
 * 3. 再用 ShmAsyncClient（自身 loop 跑在独立线程）验证回调式收发与主动关闭。
 */

#include <signal.h>

#include <any>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <shm_ipc/messages.hpp>
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/shm_client.hpp>
#include <shm_ipc/shm_server.hpp>
#include <shm_ipc/simple_pod_codec.hpp>

namespace {

constexpr const char *kTestSock = "/tmp/test-shm-app.socket";

/// 等待条件成立，最多 timeout_ms 毫秒
template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms)
{
    for (int i = 0; i * 10 < timeout_ms; ++i)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

}  // anonymous namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);

    shm::codec::SimplePodCodec::RegisterPod<Heartbeat>();
    shm::codec::SimplePodCodec::RegisterPod<ClientMsg>();

    // ------------------------------------------------------------------
    // 服务端：echo Heartbeat
    // ------------------------------------------------------------------
    shm::EventLoop server_loop;
    shm::ShmServer server(&server_loop, kTestSock, "test_server");

    std::atomic<int> conn_count{0};
    server.SetCodecFactory([] {
        return std::make_shared<shm::codec::SimplePodCodec>();
    });
    server.SetConnectionCallback([&](const comm::TcpConnPtr &) { ++conn_count; });
    server.SetCloseCallback([&](const comm::TcpConnPtr &) { --conn_count; });
    server.SetMessageCallback([](const comm::TcpConnPtr &conn,
                                 const std::any &pkg) {
        if (const auto *hb = std::any_cast<Heartbeat>(&pkg))
        {
            Heartbeat reply = *hb;
            reply.client_id = static_cast<int32_t>(conn->Id());
            assert(conn->SendCmd(reply));
        }
    });

    assert(server.Start());
    std::thread server_thread([&] { server_loop.Run(); });

    // ------------------------------------------------------------------
    // Part 1: 同步 ShmClient，5 轮请求/响应（原始字节流 + 手动 codec）
    // ------------------------------------------------------------------
    {
        shm::ShmClient client;
        assert(!client.Connected());
        assert(client.Connect(kTestSock));
        assert(client.Connected());

        shm::codec::SimplePodCodec codec;
        char buf[256];

        for (int i = 1; i <= 5; ++i)
        {
            Heartbeat hb{0, i, 1000 + i};

            size_t size = 0;
            int32_t len = codec.Encode(hb, reinterpret_cast<comm::byte *>(buf),
                                       sizeof(buf), &size);
            assert(len > 0);
            assert(client.SendMessage(buf, len) == len);

            int n = client.ReadMessage(buf, sizeof(buf), 3000);
            assert(n > 0);

            std::any pkg;
            size_t consumed = 0;
            int32_t dlen = codec.Decode(reinterpret_cast<const comm::byte *>(buf),
                                        static_cast<size_t>(n), pkg, &consumed);
            assert(dlen == n);
            const auto &out = std::any_cast<const Heartbeat &>(pkg);
            assert(out.seq == i);
            assert(out.timestamp == 1000 + i);
            assert(out.client_id > 0);  // server 回填了连接 ID
        }

        client.Close();
        assert(!client.Connected());
        std::printf("part1: sync client echo OK\n");
    }

    // ------------------------------------------------------------------
    // Part 2: ShmAsyncClient，回调式收发
    // ------------------------------------------------------------------
    {
        shm::EventLoop client_loop;
        shm::ShmAsyncClient async_client(&client_loop);

        std::atomic<int> client_msgs{0};
        std::atomic<bool> conn_ok{false};
        std::atomic<bool> closed{false};

        async_client.SetCodecFactory([] {
            return std::make_shared<shm::codec::SimplePodCodec>();
        });
        async_client.SetConnectionCallback([&](const comm::TcpConnPtr &conn) {
            conn_ok = true;
            Heartbeat hb{0, 99, 7777};
            assert(conn->SendCmd(hb));
        });
        async_client.SetMessageCallback([&](const comm::TcpConnPtr &,
                                            const std::any &pkg) {
            const auto &hb = std::any_cast<const Heartbeat &>(pkg);
            assert(hb.seq == 99);
            assert(hb.timestamp == 7777);
            ++client_msgs;
            async_client.Close();  // 回调内主动关闭
        });
        async_client.SetCloseCallback([&](const comm::TcpConnPtr &) {
            closed = true;
            client_loop.Stop();
        });

        // Connect 会注册 fd 到 client_loop，必须在 loop 线程内调用
        std::thread client_thread([&] {
            async_client.Connect(kTestSock);
            assert(async_client.State() == comm::TcpClientStates::established);
            client_loop.Run();
        });

        if (!WaitFor([&] { return client_msgs.load() >= 1; }, 3000))
            client_loop.Stop();  // 兜底，避免挂死
        client_thread.join();

        assert(client_msgs.load() >= 1);
        assert(conn_ok.load());
        assert(closed.load());
        assert(async_client.State() == comm::TcpClientStates::close);
        std::printf("part2: async client echo OK\n");
    }

    // ------------------------------------------------------------------
    // Part 3: verifing 中析构 ShmAsyncClient，挂起的验证定时器必须被摘除
    //（回归：析构曾只 RemoveFd 不 Close，验证定时器回调捕获裸 this，
    //  连接释放后触发即 use-after-free）
    // ------------------------------------------------------------------
    {
        shm::EventLoop client_loop;
        std::atomic<bool> verify_started{false};
        std::atomic<bool> timer_fired{false};
        {
            shm::ShmAsyncClient async_client(&client_loop);

            async_client.SetCodecFactory([] {
                return std::make_shared<shm::codec::SimplePodCodec>();
            });
            async_client.SetConnectionCallback([](const comm::TcpConnPtr &) {});
            async_client.SetMessageCallback([](const comm::TcpConnPtr &,
                                               const std::any &) {});
            async_client.SetCloseCallback([](const comm::TcpConnPtr &) {});
            async_client.SetVerifyCallback([&](const comm::TcpConnPtr &conn) {
                // 挂 500ms 验证超时定时器，故意不回调 OnVerify。
                // 时序：Stop 在 ~60ms 内生效（Run 用 50ms poll 粒度），
                // 定时器 500ms 才到期，保证它活到析构之后
                conn->StartVerifyTimer(500, 0, [&](int64_t) {
                    timer_fired = true;
                });
                verify_started = true;
            });

            std::thread client_thread([&] {
                async_client.Connect(kTestSock);
                client_loop.Run(50);
            });
            assert(WaitFor([&] { return verify_started.load(); }, 3000));
            client_loop.Stop();
            client_thread.join();
        }  // ~ShmAsyncClient：此刻连接仍 verifing、定时器仍挂起

        // 重启 loop 观察到 500ms 之后：若定时器未被析构摘除（bug），
        // 它会在此窗口触发，命中已释放连接的悬垂 this（ASan 下报 UAF）
        std::thread rerun_thread([&] { client_loop.Run(50); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        client_loop.Stop();
        rerun_thread.join();

        assert(!timer_fired.load());
        std::printf("part3: pending verify timer removed on client dtor\n");
    }

    // ------------------------------------------------------------------
    // Part 4: 二次 Start 不应崩溃（回归：Listen 对仍 joinable 的握手线程
    // move 赋值曾触发 std::terminate）
    // ------------------------------------------------------------------
    {
        shm::EventLoop idle_loop;
        shm::ShmServer server2(&idle_loop, "/tmp/test-shm-app-dbl.socket", "dbl");
        server2.SetCodecFactory([] {
            return std::make_shared<shm::codec::SimplePodCodec>();
        });
        server2.SetConnectionCallback([](const comm::TcpConnPtr &) {});
        server2.SetMessageCallback([](const comm::TcpConnPtr &,
                                      const std::any &) {});
        server2.SetCloseCallback([](const comm::TcpConnPtr &) {});

        assert(server2.Start());
        assert(server2.Start());  // 回归：此处曾 std::terminate
        server2.Stop();           // 幂等停止（~ShmServer 会再次调用）
        std::printf("part4: double Start() is a no-op\n");
    }

    // 等待服务端感知三条连接的关闭，再停止服务端
    assert(WaitFor([&] { return conn_count.load() == 0; }, 2000));
    server_loop.Stop();
    server_thread.join();

    std::printf("=== TEST PASSED ===\n");
    return 0;
}
