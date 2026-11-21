// 回归：畸形 fd 控制消息、尺寸封印、回调重连、过期验证、EOF/Close、注册失败回滚。
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/shm_client.hpp>
#include <shm_ipc/shm_server.hpp>

#include <dirent.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace {
thread_local int fail_after = -1;
thread_local bool allocation_failed = false;
thread_local bool fail_handshake_queue = false;
thread_local int queue_allocation = 0;
thread_local int handshake_notify_fd = -1;
}

// 仅事件循环线程启用一次性故障；握手线程和 peer 不受影响。
void* operator new(std::size_t size)
{
    if (fail_after == 0)
    {
        fail_after = -1;
        allocation_failed = true;
        throw std::bad_alloc{};
    }
    if (fail_after > 0) --fail_after;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// 专用故障点：服务器首先注册 listen fd、握手 eventfd。
// 在握手通知已就绪但回调尚未分发时，注入 deque 构造分配失败。
extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout)
{
    const int result = __real_poll(fds, count, timeout);
    if (result > 0 && fail_handshake_queue && count >= 2 && (fds[1].revents & POLLIN))
    {
        handshake_notify_fd = fds[1].fd;
        fail_handshake_queue = false;
        allocation_failed = false;
        fail_after = queue_allocation;
    }
    return result;
}

namespace {
int CountFds()
{
    DIR* dir = ::opendir("/proc/self/fd");
    assert(dir);
    int n = 0;
    while (::readdir(dir)) ++n;
    ::closedir(dir);
    return n;
}

struct SocketPair
{
    shm::UniqueFd a, b;
    SocketPair()
    {
        int fds[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        a.Reset(fds[0]); b.Reset(fds[1]);
    }
};

void TestRightsCleanup()
{
    SocketPair sockets;
    shm::UniqueFd event;
    assert(shm::CreateEventfd(&event) == shm::ErrorCode::kOk);
    for (int count : {2, 8})
    {
        const int before = CountFds();
        char data[256]{};
        alignas(cmsghdr) char control[CMSG_SPACE(8 * sizeof(int))]{};
        iovec io{data, sizeof(data)};
        msghdr msg{};
        msg.msg_iov = &io; msg.msg_iovlen = 1;
        msg.msg_control = control; msg.msg_controllen = CMSG_SPACE(count * sizeof(int));
        auto* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(count * sizeof(int));
        for (int i = 0; i < count; ++i)
        {
            const int fd = event.Get();
            std::memcpy(CMSG_DATA(c) + i * sizeof(fd), &fd, sizeof(fd));
        }
        assert(::sendmsg(sockets.a.Get(), &msg, 0) == sizeof(data));
        shm::TaggedFd received;
        const auto ec = shm::RecvFd(sockets.b.Get(), &received);
        assert(ec == (count == 2 ? shm::ErrorCode::kMultipleFds : shm::ErrorCode::kProtocol));
        assert(CountFds() == before);
    }
}

void TestSeals()
{
    using Channel = shm::RingChannel<4096>;
    shm::UniqueFd sealed;
    assert(shm::CreateMemfd("sealed", Channel::Ring::kShmSize, &sealed) == shm::ErrorCode::kOk);
    assert((::fcntl(sealed.Get(), F_GET_SEALS) & (F_SEAL_SHRINK | F_SEAL_GROW)) ==
           (F_SEAL_SHRINK | F_SEAL_GROW));
    assert(::ftruncate(sealed.Get(), 0) == -1 && errno == EPERM);
    assert(::ftruncate(sealed.Get(), Channel::Ring::kShmSize * 2) == -1 && errno == EPERM);
    for (int seals : {0, F_SEAL_SHRINK, F_SEAL_GROW})
    {
        SocketPair sockets;
        shm::UniqueFd unsealed{static_cast<int>(::syscall(__NR_memfd_create, "unsealed", MFD_ALLOW_SEALING))};
        assert(unsealed);
        assert(::ftruncate(unsealed.Get(), Channel::Ring::kShmSize) == 0);
        if (seals) assert(::fcntl(unsealed.Get(), F_ADD_SEALS, seals) == 0);
        assert(shm::SendFd(sockets.a.Get(), unsealed.Get(), shm::FdTag::kMemfd) == shm::ErrorCode::kOk);
        Channel channel;
        assert(Channel::CreateRingChannel(sockets.b.Get(), shm::ChannelRole::kServer, &channel) ==
               shm::ErrorCode::kProtocol);
        assert(channel.Readable() == 0 && channel.NotifyReadFd() == -1);
    }
}

void TestLateVerify()
{
    shm::EventLoop loop;
    auto conn = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
    conn->SetVerifyCallback([](const auto&) {});
    conn->OnConnection();
    assert(conn->State() == comm::TcpConnStates::verifing);
    conn->OnVerify(true);
    assert(conn->State() == comm::TcpConnStates::established);
    conn->Close(); conn->OnVerify(true);
    assert(conn->State() == comm::TcpConnStates::closeing);
    conn->OnClose(); conn->OnVerify(true); conn->OnVerify(false);
    assert(conn->State() == comm::TcpConnStates::closeed);
}

struct ByteCodec : comm::codec::Codec
{
    int32_t Decode(const comm::byte*, size_t n, std::any& pkg, size_t*) override
    {
        if (!n) return 0;
        pkg = 1;
        return 1;
    }
};

std::string TestPath(const char* name)
{
    return "/tmp/shm-audit-" + std::to_string(::getpid()) + "-" + name + ".socket";
}

void TestReconnect()
{
    const auto path = TestPath("reconnect");
    shm::UniqueFd listener;
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    std::thread peer([&] {
        shm::UniqueFd first{::accept(listener.Get(), nullptr, nullptr)};
        shm::DefaultRingChannel channel;
        assert(shm::DefaultRingChannel::CreateRingChannel(first.Get(), shm::ChannelRole::kServer, &channel) ==
               shm::ErrorCode::kOk);
        const char byte = 1;
        // 不通知：强制在 socket EOF 路径分发最后一包。
        assert(channel.TryWrite(&byte, 1) == 0);
        ::shutdown(first.Get(), SHUT_RDWR);
        shm::UniqueFd second{::accept(listener.Get(), nullptr, nullptr)};
        shm::DefaultRingChannel next;
        assert(shm::DefaultRingChannel::CreateRingChannel(second.Get(), shm::ChannelRole::kServer, &next) ==
               shm::ErrorCode::kOk);
        char b;
        (void)::read(second.Get(), &b, 1);
    });
    shm::EventLoop loop;
    shm::ShmAsyncClient client(&loop);
    int opens = 0, closes = 0;
    client.SetCodecFactory([] { return std::make_shared<ByteCodec>(); });
    client.SetConnectionCallback([&](const auto&) { ++opens; });
    client.SetMessageCallback([&](const auto&, const auto&) { client.Close(); });
    client.SetCloseCallback([&](const auto&) { if (++closes == 1) client.Connect(path); });
    client.Connect(path);
    int timer;
    assert(loop.AddOneShotTimer(100, [&](int, short) { loop.Stop(); }, &timer) == shm::ErrorCode::kOk);
    assert(loop.Run(10) == shm::ErrorCode::kOk);
    assert(opens == 2 && closes == 1 && client.Conn());
    client.Close(); peer.join(); ::unlink(path.c_str());
}

void TestEofClose()
{
    const auto path = TestPath("eof");
    shm::UniqueFd listener;
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    std::atomic<bool> connected{false}, stop{false};
    std::thread peer([&] {
        shm::UniqueFd fd{::accept(listener.Get(), nullptr, nullptr)};
        shm::DefaultRingChannel channel;
        assert(shm::DefaultRingChannel::CreateRingChannel(fd.Get(), shm::ChannelRole::kServer, &channel) ==
               shm::ErrorCode::kOk);
        while (!connected.load()) std::this_thread::yield();
        ::shutdown(fd.Get(), SHUT_RDWR);
    });
    shm::ShmClient client;
    assert(client.Connect(path));
    const int fd = client.GetSockFd();
    connected.store(true);
    char b;
    assert(client.ReadMessage(&b, 1, 1000) == 0);
    assert(!client.Connected() && client.GetSockFd() == fd);
    assert(client.ReadMessage(&b, 1, 0) == 0); // 对端 EOF 保持可重复读取
    client.Close(); // 本端关闭后读取才返回 -1
    std::thread closer([&] { while (!stop.load()) client.Close(); });
    for (int i = 0; i < 1000; ++i) assert(client.ReadMessage(&b, 1, 0) == -1);
    stop.store(true); closer.join(); peer.join(); ::unlink(path.c_str());
}

void TestRegistrationRollback()
{
    int failures = 0;
    for (int allocation = 0; allocation < 16; ++allocation)
    {
        const int before = CountFds();
        {
            const auto path = TestPath("oom");
            shm::EventLoop loop;
            // 填充 deque，使连接注册也会遇到节点分配。
            for (int i = 0; i < 8; ++i) loop.AddFd(-1, [](int, short) {});
            shm::ShmServer server(&loop, path);
            bool attempted = false;
            int established = 0;
            server.SetConnectionCallback([&](const auto&) { ++established; });
            server.SetMessageCallback([](const auto&, const auto&) {});
            server.SetCloseCallback([](const auto&) {});
            server.SetCodecFactory([&] {
                auto codec = std::make_shared<ByteCodec>();
                attempted = true; allocation_failed = false; fail_after = allocation;
                return codec;
            });
            shm::EventLoop::HookId hook;
            assert(loop.AddPostDispatchHook([&] {
                fail_after = -1;
                if (attempted)
                {
                    if (allocation_failed) ++failures;
                    assert(server.ConnCount() == static_cast<size_t>(established));
                    loop.Stop();
                }
            }, &hook) == shm::ErrorCode::kOk);
            assert(server.Start());
            std::thread peer([&] {
                shm::UniqueFd fd;
                assert(shm::ConnectUnixSocket(path, &fd) == shm::ErrorCode::kOk);
                shm::DefaultRingChannel channel;
                const auto ec = shm::DefaultRingChannel::CreateRingChannel(fd.Get(), shm::ChannelRole::kClient, &channel);
                assert(ec == shm::ErrorCode::kOk);
                char b;
                (void)::read(fd.Get(), &b, 1);
            });
            assert(loop.Run(10) == shm::ErrorCode::kOk);
            assert(attempted);
            server.Stop(); peer.join();
        }
        assert(CountFds() == before);
    }
    assert(failures > 0);
}
void TestHandshakeQueueRecovery()
{
    for (int allocation : {0, 1}) // deque 控制表及首个节点分配
    {
        const int before = CountFds();
        {
            const auto path = TestPath("queue-oom");
            shm::EventLoop loop;
            shm::ShmServer server(&loop, path);
            int established = 0;
            server.SetConnectionCallback([&](const auto&) { ++established; loop.Stop(); });
            server.SetMessageCallback([](const auto&, const auto&) {});
            server.SetCloseCallback([](const auto&) {});
            server.SetCodecFactory([] { return std::make_shared<ByteCodec>(); });
            assert(server.Start());
            int timer;
            assert(loop.AddOneShotTimer(2000, [&](int, short) { loop.Stop(); }, &timer) == shm::ErrorCode::kOk);
            std::atomic<bool> client_connected{false};
            queue_allocation = allocation;
            fail_handshake_queue = true;
            handshake_notify_fd = -1;
            std::thread peer([&] {
                shm::UniqueFd sock;
                assert(shm::ConnectUnixSocket(path, &sock) == shm::ErrorCode::kOk);
                shm::DefaultRingChannel channel;
                assert(shm::DefaultRingChannel::CreateRingChannel(sock.Get(), shm::ChannelRole::kClient, &channel) == shm::ErrorCode::kOk);
                client_connected.store(true);
                char b; (void)::read(sock.Get(), &b, 1);
            });
            assert(loop.Run(-1) == shm::ErrorCode::kNoMemory);
            assert(allocation_failed && !fail_handshake_queue && handshake_notify_fd >= 0);
            while (!client_connected.load()) std::this_thread::yield();
            assert(established == 0 && server.ConnCount() == 0);
            // 分配失败时不确认通知，eventfd 必须继续就绪。
            pollfd notification{handshake_notify_fd, POLLIN, 0};
            assert(::poll(&notification, 1, 0) == 1 && (notification.revents & POLLIN));
            fail_after = -1;
            assert(loop.Run(-1) == shm::ErrorCode::kOk);
            assert(established == 1 && server.ConnCount() == 1); // 没有第二个客户端或重新通知
            loop.RemoveTimer(timer);
            server.Stop(); peer.join();
        }
        assert(CountFds() == before);
    }
}

}  // namespace

int main()
{
    TestRightsCleanup(); TestSeals(); TestLateVerify(); TestReconnect(); TestEofClose(); TestRegistrationRollback(); TestHandshakeQueueRecovery();
    std::puts("=== AUDIT REGRESSION PASSED ===");
}
