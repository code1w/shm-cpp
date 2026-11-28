#include <shm_ipc/shm_server.hpp>
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/simple_pod_codec.hpp>
#include <shm_ipc/messages.hpp>
#include <pthread.h>
#include <csignal>
#include <dirent.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
using E = shm::ErrorCode;
#define CHECK(x)                                                 \
    do                                                           \
    {                                                            \
        if (!(x))                                                \
        {                                                        \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            std::abort();                                        \
        }                                                        \
    } while (0)
static thread_local unsigned fail_after = 0;
extern "C" void* __real__ZnwmRKSt9nothrow_t(size_t, const std::nothrow_t&);
extern "C" void* __wrap__ZnwmRKSt9nothrow_t(size_t n, const std::nothrow_t& tag) noexcept
{
    if (fail_after && --fail_after == 0)
        return nullptr;
    return __real__ZnwmRKSt9nothrow_t(n, tag);
}
static size_t Fds()
{
    DIR* d = ::opendir("/proc/self/fd");
    CHECK(d);
    size_t n = 0;
    while (auto* e = ::readdir(d))
        if (e->d_name[0] != '.')
            ++n;
    ::closedir(d);
    return n;
}
static void* Run(void* ctx)
{
    CHECK(static_cast<shm::EventLoop*>(ctx)->Run(20) == E::kOk);
    return nullptr;
}
int main()
{
    ::alarm(20);
    ::signal(SIGPIPE, SIG_IGN);
    char path[100];
    std::snprintf(path, sizeof(path), "/tmp/shm-reg-app-%ld.sock", long(::getpid()));
    const auto before = Fds();
    for (unsigned failure : {1u, 2u})
    {
        {
            shm::EventLoop loop;
            shm::ShmServer server(&loop, path);
            server.SetConnectionCallback([](const comm::TcpConnPtr&) {});
            server.SetCloseCallback([](const comm::TcpConnPtr&) {});
            server.SetMessageCallback([](const comm::TcpConnPtr&, const std::any&) {});
            server.SetCodecFactory([] { return std::make_shared<shm::codec::SimplePodCodec>(); });
            fail_after = failure;
            CHECK(!server.Start() && server.LastError() == E::kNoMemory);
            CHECK(::access(path, F_OK) != 0);
            // The next Start must work after either watch-registration rollback.
            CHECK(server.Start());
            server.Stop();
        }
        CHECK(Fds() == before);
    }
    shm::codec::SimplePodCodec::RegisterPod<Heartbeat>();
    for (bool busy : {false, true})
    {
        {
            shm::EventLoop server_loop;
            CHECK(server_loop.SetBusyPolling(busy) == E::kOk);
            shm::ShmServer server(&server_loop, path);
            server.SetConnectionCallback([](const comm::TcpConnPtr&) {});
            server.SetCloseCallback([](const comm::TcpConnPtr&) {});
            server.SetCodecFactory([] { return std::make_shared<shm::codec::SimplePodCodec>(); });
            server.SetMessageCallback(
                [](const comm::TcpConnPtr& c, const std::any& message) { CHECK(c->SendCmd(message)); });
            CHECK(server.Start());
            pthread_t worker;
            CHECK(::pthread_create(&worker, nullptr, Run, &server_loop) == 0);
            {
                shm::EventLoop client_loop;
                CHECK(client_loop.SetBusyPolling(busy) == E::kOk);
                shm::ShmAsyncClient client(&client_loop);
                unsigned connected = 0, errors = 0, closed = 0, replies = 0;
                client.SetCodecFactory([] { return std::make_shared<shm::codec::SimplePodCodec>(); });
                client.SetConnectErrorCallback([&](bool, const std::string&) { ++errors; });
                client.SetConnectionCallback([&](const comm::TcpConnPtr& c) {
                    ++connected;
                    CHECK(c->SendCmd(Heartbeat{0, 7, 99}));
                });
                client.SetMessageCallback([&](const comm::TcpConnPtr&, const std::any& a) {
                    const auto* p = std::any_cast<Heartbeat>(&a);
                    CHECK(p && p->seq == 7);
                    ++replies;
                    client_loop.Stop();
                });
                client.SetCloseCallback([&](const comm::TcpConnPtr&) { ++closed; });
                // TLS injection prevents the server handshake thread from taking
                // the client's first or second registration failure.
                for (unsigned failure : {1u, 2u})
                {
                    fail_after = failure;
                    client.Connect(path);
                    CHECK(client.LastError() == E::kNoMemory && connected == 0 && closed == 0);
                }
                CHECK(errors == 2);
                client.Connect(path);
                CHECK(client.LastError() == E::kOk && connected == 1);
                CHECK(client_loop.Run(20) == E::kOk && replies == 1);
                client.Close();
                CHECK(closed == 1);
            }
            server_loop.Stop();
            CHECK(::pthread_join(worker, nullptr) == 0);
            server.Stop();
        }
        CHECK(Fds() == before && ::access(path, F_OK) != 0);
    }
    std::puts("PASS registration failure rollback and reconnect for server, async eventfd/busy clients");
}
