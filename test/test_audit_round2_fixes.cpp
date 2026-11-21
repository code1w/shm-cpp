#include <shm_ipc/shm_server.hpp>
#include <shm_ipc/shm_client.hpp>
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>
#include <cstring>

void OneShotDrain()
{
    shm::EventLoop loop;
    int t;
    assert(loop.AddOneShotTimer(
               1,
               [&](int fd, short) {
                   fprintf(stderr, "entered one-shot callback\n");
                   assert(shm::EventLoop::DrainTimerfd(fd) == 0);
                   loop.Stop();
               },
               &t) == shm::ErrorCode::kOk);
    (void)loop.Run(-1);
}
void Configure(shm::ShmServer& s)
{
    s.SetConnectionCallback([](const auto&) {});
    s.SetMessageCallback([](const auto&, const auto&) {});
    s.SetCloseCallback([](const auto&) {});
    s.SetCodecFactory([] { return std::make_shared<comm::codec::Codec>(); });
}
void HeadOfLine()
{
    shm::EventLoop loop;
    std::string path = "/tmp/shm-review-hol-" + std::to_string(getpid());
    shm::ShmServer server(&loop, path);
    Configure(server);
    assert(server.Start());
    shm::UniqueFd idle1, idle2;
    assert(shm::ConnectUnixSocket(path, &idle1) == shm::ErrorCode::kOk);
    assert(shm::ConnectUnixSocket(path, &idle2) == shm::ErrorCode::kOk);
    std::thread healthy([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        shm::ShmClient c;
        auto start = std::chrono::steady_clock::now();
        bool ok    = c.Connect(path);
        assert(ok);
        assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        fprintf(
            stderr, "healthy client connected=%d, error=%d, elapsed_ms=%lld\n", ok, static_cast<int>(c.LastError()),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                .count());
        loop.Stop();
    });
    (void)loop.Run(-1);
    healthy.join();
    server.Stop();
}
struct ReentrantCodec : comm::codec::Codec
{
    shm::ShmServer* server;
    int length;
    bool stop_all;
    explicit ReentrantCodec(shm::ShmServer* s, int n, bool stop) : server(s), length(n), stop_all(stop) {}
    int32_t EncodedSize(const std::any&) const override { return length; }
    int32_t Encode(const std::any&, comm::byte* buf, size_t, size_t*) override
    {
        if (stop_all)
            server->Stop();
        else
            server->CloseConn(conn_id);
        memset(buf, 42, length);
        return length;
    }
};
void BroadcastReentry(int length, bool stop_all = false)
{
    shm::EventLoop loop;
    std::string path = "/tmp/shm-review-broadcast-" + std::to_string(getpid());
    shm::ShmServer server(&loop, path);
    Configure(server);
    auto codec = std::make_shared<ReentrantCodec>(&server, length, stop_all);
    server.SetCodecFactory([&] { return codec; });
    server.SetConnectionCallback([&](const auto&) {
        if (server.ConnCount() < (stop_all ? 2u : 1u))
            return;
        int t;
        assert(loop.AddOneShotTimer(
                   10,
                   [&](int, short) {
                       server.Broadcast(std::any{});
                       loop.Stop();
                   },
                   &t) == shm::ErrorCode::kOk);
    });
    assert(server.Start());
    std::atomic<bool> done{false};
    auto connect = [&] {
        shm::ShmClient c;
        assert(c.Connect(path));
        while (!done.load())
            std::this_thread::yield();
        assert(c.Channel().Readable() == 0);
    };
    std::thread peer(connect);
    std::thread second;
    if (stop_all)
        second = std::thread(connect);
    (void)loop.Run(-1);
    assert(server.ConnCount() == 0);
    done = true;
    peer.join();
    if (second.joinable())
        second.join();
    server.Stop();
}

static int gate_fd = -1;
static std::atomic<int> accepted{0}, complete{0};
extern "C" int __real_accept4(int, sockaddr*, socklen_t*, int);
extern "C" int __wrap_accept4(int fd, sockaddr* addr, socklen_t* len, int flags)
{
    int c = __real_accept4(fd, addr, len, flags);
    if (c >= 0 && gate_fd >= 0 && ++accepted == 2)
    {
        uint64_t one = 1;
        assert(write(gate_fd, &one, 8) == 8);
    }
    return c;
}
void BatchRestart()
{
    shm::EventLoop loop;
    shm::UniqueFd gate;
    assert(shm::CreateEventfd(&gate) == shm::ErrorCode::kOk);
    gate_fd  = gate.Get();
    accepted = 0;
    complete = 0;
    // 两次 accept 后优先处理该 fd，等待两个后台握手结束，模拟 loop 繁忙时通知合并。
    loop.AddFd(gate_fd, [&](int fd, short) {
        shm::DefaultRingChannel::DrainNotify(fd);
        while (complete.load() < 2)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    std::string path = "/tmp/shm-review-batch-" + std::to_string(getpid());
    shm::ShmServer server(&loop, path);
    int callbacks = 0;
    server.SetMessageCallback([](const auto&, const auto&) {});
    server.SetCloseCallback([](const auto&) {});
    server.SetCodecFactory([] { return std::make_shared<comm::codec::Codec>(); });
    server.SetConnectionCallback([&](const auto&) {
        ++callbacks;
        if (callbacks == 1)
        {
            server.Stop();
            assert(server.Start());
        }
    });
    assert(server.Start());
    std::atomic<bool> done{false};
    auto connect = [&] {
        shm::ShmClient c;
        assert(c.Connect(path));
        ++complete;
        while (!done.load())
            std::this_thread::yield();
    };
    std::thread a(connect), b(connect);
    int t;
    assert(loop.AddOneShotTimer(
               500, [&](int, short) { loop.Stop(); }, &t) == shm::ErrorCode::kOk);
    (void)loop.Run(-1);
    assert(callbacks == 1 && server.ConnCount() == 0);
    done = true;
    a.join();
    b.join();
    server.Stop();
    gate_fd = -1;
}

void NotifyEventfd()
{
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    shm::UniqueFd client_sock{sockets[0]}, server_sock{sockets[1]}, ring;
    shm::MmapRegion region;
    assert(shm::CreateMemfd("notify-flags", shm::DefaultRingBuf::kShmSize, &ring) == shm::ErrorCode::kOk);
    assert(region.Map(ring.Get(), shm::DefaultRingBuf::kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED) ==
           shm::ErrorCode::kOk);
    shm::DefaultRingBuf::Init(region.Get());
    shm::UniqueFd reader;
    assert(shm::CreateEventfd(&reader) == shm::ErrorCode::kOk);
    std::thread peer([&] {
        shm::TaggedFd incoming;
        assert(shm::RecvFd(server_sock.Get(), &incoming) == shm::ErrorCode::kOk);
        assert(shm::RecvFd(server_sock.Get(), &incoming) == shm::ErrorCode::kOk);
        assert(incoming.tag == shm::FdTag::kEventfd);
        assert(shm::SendFd(server_sock.Get(), ring.Get(), shm::FdTag::kMemfd) == shm::ErrorCode::kOk);
        assert(shm::SendFd(server_sock.Get(), reader.Get(), shm::FdTag::kEventfd) == shm::ErrorCode::kOk);
    });
    shm::DefaultRingChannel channel;
    assert(shm::DefaultRingChannel::CreateRingChannel(client_sock.Get(), shm::ChannelRole::kClient, &channel) ==
           shm::ErrorCode::kOk);
    peer.join();
    assert(shm::ValidateNotifyEventfd(channel.NotifyReadFd()) == shm::ErrorCode::kOk);
    assert(::fcntl(channel.NotifyReadFd(), F_GETFD) & FD_CLOEXEC);
    // 共享 eventfd 保持非阻塞；饱和时通知返回，已有计数仍能唤醒读方。
    const uint64_t full = UINT64_MAX - 1;
    assert(::write(reader.Get(), &full, sizeof(full)) == sizeof(full));
    auto start = std::chrono::steady_clock::now();
    assert(channel.TryWrite("a", 1) == 0);
    channel.NotifyPeer();
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    assert(shm::DefaultRingChannel::DrainNotify(reader.Get()) == full);
    assert(shm::DefaultRingChannel::DrainNotify(reader.Get()) == 0);
    channel.NotifyPeer();
    channel.NotifyPeer();
    assert(shm::DefaultRingChannel::DrainNotify(reader.Get()) == 2);
    channel.WakeReader();
    assert(shm::DefaultRingChannel::DrainNotify(channel.NotifyReadFd()) == 1);
    assert(shm::DefaultRingChannel::DrainNotify(channel.NotifyReadFd()) == 0);
    const int flags = ::fcntl(reader.Get(), F_GETFL);
    assert(flags & O_NONBLOCK);
    assert(::fcntl(reader.Get(), F_SETFL, flags & ~O_NONBLOCK) == 0);
    assert(shm::ValidateNotifyEventfd(reader.Get()) == shm::ErrorCode::kProtocol);
    assert(::fcntl(reader.Get(), F_SETFL, flags) == 0);  // 未恢复前不得调用通知 I/O
    int wrong[2];
    assert(::pipe2(wrong, O_CLOEXEC | O_NONBLOCK) == 0);
    shm::UniqueFd pipe_read{wrong[0]}, pipe_write{wrong[1]};
    assert(shm::ValidateNotifyEventfd(pipe_read.Get()) == shm::ErrorCode::kProtocol);
}
struct ResizeDuringEncodeCodec : comm::codec::Codec
{
    std::weak_ptr<shm::ShmConnection> connection;
    int calls = 0;
    int32_t Encode(const std::any&, comm::byte* buffer, size_t capacity, size_t*) override
    {
        if (calls++)
            assert(capacity >= 512 * 1024);
        connection.lock()->ResizeBuffer(512 * 1024, 1024 * 1024);
        buffer[0] = 42;  // ResizeBuffer 返回后，当前借用指针仍须有效
        return 1;
    }
};
void ResizeDuringEncode()
{
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    shm::UniqueFd a{sockets[0]}, b{sockets[1]};
    shm::DefaultRingChannel client, server;
    std::thread peer([&] {
        assert(shm::DefaultRingChannel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &server) ==
               shm::ErrorCode::kOk);
    });
    assert(shm::DefaultRingChannel::CreateRingChannel(a.Get(), shm::ChannelRole::kClient, &client) ==
           shm::ErrorCode::kOk);
    peer.join();
    shm::EventLoop loop;
    auto conn         = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(a), std::move(client));
    auto codec        = std::make_shared<ResizeDuringEncodeCodec>();
    codec->connection = conn;
    conn->SetCodec(codec);
    conn->OnConnection();
    assert(conn->SendCmd(1));
    assert(conn->SendCmd(2));
    unsigned char received[2]{};
    assert(server.ReadExact(received, sizeof(received)) == 0);
    assert(received[0] == 42 && received[1] == 42 && codec->calls == 2);
}

int main()
{
    ResizeDuringEncode();
    OneShotDrain();
    BroadcastReentry(1);
    BroadcastReentry(256);
    BroadcastReentry(1, true);
    HeadOfLine();
    BatchRestart();
    NotifyEventfd();
    std::puts("=== ROUND TWO FIX REGRESSION PASSED ===");
}
