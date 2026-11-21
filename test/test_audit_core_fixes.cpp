#include <shm_ipc/shm_server.hpp>
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>
static pthread_t main_thread;
static std::atomic<bool> interrupt_notify{false};
static std::atomic<int> interrupted{0};
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t n)
{
    if (n == 8 && !pthread_equal(pthread_self(), main_thread) && interrupt_notify.exchange(false))
    {
        ++interrupted;
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, data, n);
}

void TimerZero()
{
    int f[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, f) == 0);
    shm::UniqueFd a(f[0]), b(f[1]);
    shm::DefaultRingChannel x, y;
    std::thread hs([&] {
        assert(shm::DefaultRingChannel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &y) ==
               shm::ErrorCode::kOk);
    });
    assert(shm::DefaultRingChannel::CreateRingChannel(a.Get(), shm::ChannelRole::kClient, &x) == shm::ErrorCode::kOk);
    hs.join();
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(a), std::move(x));
    c->SetVerifyCallback([](const auto&) {});
    c->OnConnection();
    int saved = dup(0);
    close(0);
    int fired = 0;
    c->StartVerifyTimer(1, 0, [&](int64_t) { ++fired; });
    assert(::fcntl(0, F_GETFD) >= 0);
    c->StartVerifyTimer(1, 0, [&](int64_t) { ++fired; });
    c->OnVerify(true);
    assert(::fcntl(0, F_GETFD) < 0 && errno == EBADF);
    int t;
    assert(loop.AddOneShotTimer(
               20, [&](int, short) { loop.Stop(); }, &t) == shm::ErrorCode::kOk);
    (void)loop.Run(-1);
    assert(fired == 0);
    if (saved >= 0)
    {
        dup2(saved, 0);
        close(saved);
    }
}
void FactoryStop(bool stop, bool restart = false)
{
    shm::EventLoop loop;
    std::string path = "/tmp/shm-review-factory-" + std::to_string(getpid());
    shm::ShmServer server(&loop, path);
    int connected = 0;
    server.SetConnectionCallback([&](const auto&) {
        ++connected;
        loop.Stop();
    });
    server.SetMessageCallback([](const auto&, const auto&) {});
    server.SetCloseCallback([](const auto&) {});
    server.SetCodecFactory([&] {
        if (stop)
        {
            server.Stop();
            if (restart)
                assert(server.Start());
        }
        return std::make_shared<comm::codec::Codec>();
    });
    assert(server.Start());
    std::thread client([&] {
        shm::UniqueFd s;
        assert(shm::ConnectUnixSocket(path, &s) == shm::ErrorCode::kOk);
        shm::DefaultRingChannel ch;
        assert(shm::DefaultRingChannel::CreateRingChannel(s.Get(), shm::ChannelRole::kClient, &ch) ==
               shm::ErrorCode::kOk);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    });
    int t;
    assert(loop.AddOneShotTimer(
               500, [&](int, short) { loop.Stop(); }, &t) == shm::ErrorCode::kOk);
    (void)loop.Run(-1);
    client.join();
    fprintf(stderr, "connection callback after Stop in factory: %d, remaining connections: %zu\n", connected,
            server.ConnCount());
    assert(connected == (stop ? 0 : 1));
    assert(server.ConnCount() == (stop ? 0u : 1u));
    server.CloseConn(1);
}
void BacklogTimeout()
{
    const std::string path = "/tmp/shm-review-backlog-" + std::to_string(getpid());
    shm::UniqueFd listener, a, b, out;
    assert(shm::ListenUnixSocket(path, &listener, 1) == shm::ErrorCode::kOk);
    assert(shm::ConnectUnixSocket(path, &a) == shm::ErrorCode::kOk);
    assert(shm::ConnectUnixSocket(path, &b) == shm::ErrorCode::kOk);
    int sys_errno = 0;
    const shm::detail::IoDeadline deadline{100};
    assert(shm::ConnectUnixSocket(path, &out, &sys_errno, &deadline) == shm::ErrorCode::kTimeout);
    assert(sys_errno == ETIMEDOUT && !out);
    assert(std::chrono::steady_clock::now() - deadline.end < std::chrono::seconds(1));
    // 队列释放后重试可成功，返回 socket 保持历史的阻塞语义。
    shm::UniqueFd accepted{::accept(listener.Get(), nullptr, nullptr)};
    assert(accepted);
    assert(shm::ConnectUnixSocket(path, &out) == shm::ErrorCode::kOk);
    assert(!(::fcntl(out.Get(), F_GETFL) & O_NONBLOCK));
    const shm::detail::IoDeadline expired{0};
    shm::DefaultRingChannel channel;
    assert(shm::DefaultRingChannel::CreateRingChannel(out.Get(), shm::ChannelRole::kClient, &channel, &sys_errno,
                                                      &expired) == shm::ErrorCode::kTimeout);
    assert(sys_errno == ETIMEDOUT);
    assert(shm::UnlinkUnixSocket(path));
}
int main()
{
    main_thread = pthread_self();
    TimerZero();
    FactoryStop(true);
    FactoryStop(true, true);
    interrupt_notify.store(true);
    FactoryStop(false);
    assert(interrupted == 1 && !interrupt_notify.load());
    BacklogTimeout();
    std::puts("=== CORE REVIEW FIX REGRESSION PASSED ===");
}
