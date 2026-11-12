/**
 * @file bench_app.cpp
 * @brief 上层接口吞吐基准：ShmServer + ShmAsyncClient + SimplePodCodec
 *
 * 服务端对每个 Heartbeat 请求回一个 Heartbeat；客户端按批发送请求，
 * 收到本批全部响应后发下一批。通知由 reader_wait 按需抑制：
 * 读方处理中不写 eventfd，仅在其睡眠时通知。
 *
 * 用法：
 *   ./bench_app [batch_size] [--big|--huge]
 */

#include <shm_ipc/bench_common.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/shm_server.hpp>
#include <shm_ipc/simple_pod_codec.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

/// 大对象 POD（4KB / 1MB），用于验证上层大消息与 any 复用
struct BenchBigPod
{
    uint64_t seq;
    uint32_t len;
    uint8_t  data[4096];
};
SHM_IPC_REGISTER_POD(BenchBigPod, 0x42494750);

struct BenchHugePod
{
    uint64_t seq;
    uint32_t len;
    uint8_t  data[1024 * 1024];
};
SHM_IPC_REGISTER_POD(BenchHugePod, 0x48554750);

namespace {

enum class BenchKind
{
    kSmall,
    kBig4K,
    kHuge1M,
};

int        g_batch     = 64;
BenchKind  g_kind      = BenchKind::kSmall;
int        g_total     = 1000000;

// 客户端发送复用 any（只在客户端事件循环线程使用）
std::any g_client_any;

std::size_t KindPayloadSize()
{
    switch (g_kind)
    {
        case BenchKind::kBig4K:  return sizeof(BenchBigPod);
        case BenchKind::kHuge1M: return sizeof(BenchHugePod);
        default:                 return sizeof(Heartbeat);
    }
}

const char *KindName()
{
    switch (g_kind)
    {
        case BenchKind::kBig4K:  return "big4k";
        case BenchKind::kHuge1M: return "huge1m";
        default:                 return "small";
    }
}

std::atomic<int>  g_replies{0};
std::atomic<int>  g_sent{0};
std::atomic<bool> g_done{false};
uint64_t          g_t0 = 0;

uint64_t NowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 客户端发送请求（复用 any，避免大对象每条消息重新分配）
void SendRequest(const comm::TcpConnPtr &conn, int seq)
{
    if (g_kind == BenchKind::kHuge1M)
    {
        auto *p = std::any_cast<BenchHugePod>(&g_client_any);
        if (!p)
        {
            g_client_any = BenchHugePod{};
            p = std::any_cast<BenchHugePod>(&g_client_any);
        }
        p->seq  = static_cast<uint64_t>(seq);
        p->len  = sizeof(p->data);
        p->data[0] = 'H';
        conn->SendCmd(g_client_any);
    }
    else if (g_kind == BenchKind::kBig4K)
    {
        auto *p = std::any_cast<BenchBigPod>(&g_client_any);
        if (!p)
        {
            g_client_any = BenchBigPod{};
            p = std::any_cast<BenchBigPod>(&g_client_any);
        }
        p->seq  = static_cast<uint64_t>(seq);
        p->len  = sizeof(p->data);
        std::memset(p->data, 'B', sizeof(p->data));
        conn->SendCmd(g_client_any);
    }
    else
    {
        auto *p = std::any_cast<Heartbeat>(&g_client_any);
        if (!p)
        {
            g_client_any = Heartbeat{};
            p = std::any_cast<Heartbeat>(&g_client_any);
        }
        p->seq       = seq;
        p->timestamp = 1000 + seq;
        conn->SendCmd(g_client_any);
    }
}

/// 服务端回包：直接复用收到的 any，不再复制到第二个 any
void SendReply(const comm::TcpConnPtr &conn, const std::any &pkg)
{
    conn->SendCmd(pkg);
}

}  // namespace

int main(int argc, char *argv[])
{
    ::signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--big") == 0)
            g_kind = BenchKind::kBig4K;
        else if (std::strcmp(argv[i], "--huge") == 0)
            g_kind = BenchKind::kHuge1M;
        else
            g_batch = std::max(1, std::atoi(argv[i]));
    }
    if (g_kind == BenchKind::kBig4K)
        g_total = 200000;  // 4KB × 200k = 800MB
    else if (g_kind == BenchKind::kHuge1M)
    {
        g_total = 2000;  // 1MB × 2000 = 2GB
        g_batch = std::min(g_batch, 4);  // 8MB ring 同时最多 4 条 1MB
    }

    shm::codec::SimplePodCodec::RegisterPod<Heartbeat>();
    shm::codec::SimplePodCodec::RegisterPod<BenchBigPod>();
    shm::codec::SimplePodCodec::RegisterPod<BenchHugePod>();

    shm::EventLoop server_loop;
    shm::ShmServer server(&server_loop, "/tmp/bench-app.socket", "bench_app");
    server.SetCodecFactory([] {
        return std::make_shared<shm::codec::SimplePodCodec>();
    });
    server.SetConnectionCallback([](const comm::TcpConnPtr &) {});
    server.SetCloseCallback([](const comm::TcpConnPtr &) {});
    server.SetMessageCallback([](const comm::TcpConnPtr &conn,
                                 const std::any &pkg) {
        SendReply(conn, pkg);  // 每条请求回一个响应
    });
    if (!server.Start())
        return 1;

    shm::EventLoop client_loop;
    shm::ShmAsyncClient client(&client_loop);
    client.SetCodecFactory([] {
        return std::make_shared<shm::codec::SimplePodCodec>();
    });

    client.SetConnectionCallback([&](const comm::TcpConnPtr &conn) {
        g_t0 = NowNs();
        int count = std::min(g_batch, g_total);
        for (int i = 0; i < count; ++i)
            SendRequest(conn, i);
        g_sent.store(count);
    });

    client.SetMessageCallback([&](const comm::TcpConnPtr &conn,
                                  const std::any &) {
        int n = g_replies.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n >= g_total)
        {
            uint64_t elapsed = NowNs() - g_t0;
            double   secs    = static_cast<double>(elapsed) / 1e9;
            double   mbps    = static_cast<double>(g_total) * KindPayloadSize() /
                            secs / (1024.0 * 1024.0);
            std::printf(
                "type=%-6s batch=%4d total=%d time=%7.3fs "
                "rate=%9.0f msg/s  %8.1f MB/s\n",
                KindName(), g_batch, g_total, secs,
                g_total / secs, mbps);
            g_done.store(true, std::memory_order_release);
            client_loop.Stop();
            server_loop.Stop();
            return;
        }
        if (n % g_batch == 0)
        {
            int base = g_sent.load();
            int count = std::min(g_batch, g_total - base);
            for (int i = 0; i < count; ++i)
                SendRequest(conn, base + i);
            g_sent.fetch_add(count);
        }
    });

    client.SetCloseCallback([&](const comm::TcpConnPtr &) {
        client_loop.Stop();
        server_loop.Stop();
    });
    client.SetConnectErrorCallback([&](bool, const std::string &uri) {
        std::fprintf(stderr, "bench_app: connect failed: %s\n", uri.c_str());
        g_done.store(true, std::memory_order_release);
        server_loop.Stop();
    });

    std::thread server_thread([&] { (void)server_loop.Run(50); });
    std::thread client_thread([&] {
        client.Connect("/tmp/bench-app.socket");
        (void)client_loop.Run(50);
    });

    for (int i = 0; i < 6000 && !g_done.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    client_loop.Stop();
    server_loop.Stop();
    client_thread.join();
    server_thread.join();
    client.Close();
    server.Stop();
    ::unlink("/tmp/bench-app.socket");

    if (!g_done.load(std::memory_order_acquire))
    {
        std::fprintf(stderr, "bench_app: timeout (%d/%d replies)\n",
                     g_replies.load(), g_total);
        return 1;
    }
    return 0;
}
