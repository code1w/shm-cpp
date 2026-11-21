#include <shm_ipc/proto_codec.hpp>
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/shm_server.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
std::string Path(const char* name)
{
    return "/tmp/shm-second-" + std::to_string(::getpid()) + "-" + name + ".socket";
}
void Configure(shm::ShmServer& server)
{
    server.SetConnectionCallback([](const auto&) {});
    server.SetMessageCallback([](const auto&, const auto&) {});
    server.SetCloseCallback([](const auto&) {});
    server.SetCodecFactory([] { return std::make_shared<comm::codec::Codec>(); });
}
void TestTimerIdentity()
{
    for (bool throws : {false, true})
    {
        shm::EventLoop loop;
        int old_fd = -1, new_fd = -1, fired = 0;
        assert(loop.AddOneShotTimer(1, [&](int fd, short) {
            loop.RemoveTimer(fd);
            assert(loop.AddOneShotTimer(1, [&](int, short) { ++fired; loop.Stop(); }, &new_fd) == shm::ErrorCode::kOk);
            assert(fd == new_fd); // 确保实际复用了旧 fd
            if (throws) throw std::runtime_error("timer callback");
        }, &old_fd) == shm::ErrorCode::kOk);
        if (throws)
        {
            bool caught = false;
            try { (void)loop.Run(-1); } catch (const std::runtime_error&) { caught = true; }
            assert(caught && ::fcntl(new_fd, F_GETFD) >= 0);
            assert(loop.Run(-1) == shm::ErrorCode::kOk);
        }
        else
        {
            assert(loop.Run(-1) == shm::ErrorCode::kOk);
        }
        assert(fired == 1);
    }
}
void TestStopWakeup()
{
    shm::EventLoop loop;
    for (int i = 0; i < 3; ++i)
    {
        std::atomic<bool> entered{false};
        int timer;
        assert(loop.AddOneShotTimer(1, [&](int, short) { entered.store(true); }, &timer) == shm::ErrorCode::kOk);
        std::thread stop([&] {
            while (!entered.load()) std::this_thread::yield();
            errno = EDOM;
            loop.Stop();
            assert(errno == EDOM);
        });
        assert(loop.Run(-1) == shm::ErrorCode::kOk);
        stop.join();
    }
}
struct Message
{
    uint64_t size = 1;
    bool success = true;
    std::string name = "abc";
    std::string GetTypeName() const { return name; }
    uint64_t ByteSizeLong() const { return size; }
    bool SerializeToArray(void* data, int length) const
    {
        if (!success) return false;
        assert(length >= 0 && static_cast<uint64_t>(length) == size);
        if (length) std::memset(data, 'x', length);
        return true;
    }
    bool ParseFromArray(const void*, int) { return true; }
};
void TestProtoSizes()
{
    unsigned char buf[32];
    for (uint64_t size : {static_cast<uint64_t>(INT32_MAX) + 1, static_cast<uint64_t>(UINT32_MAX) - 1,
                          static_cast<uint64_t>(UINT32_MAX) + 1})
    {
        Message msg; msg.size = size;
        std::memset(buf, 0xa5, sizeof(buf));
        assert(shm::ProtoCodec<Message>::EncodeTo(msg, buf, 11, 1) == 0);
        for (auto byte : buf) assert(byte == 0xa5);
    }
    Message msg; msg.name.assign(UINT16_MAX + 1u, 'a');
    assert(shm::ProtoCodec<Message>::EncodeTo(msg, buf, sizeof(buf), 1) == 0);
    msg = Message{}; msg.success = false;
    assert(shm::ProtoCodec<Message>::EncodeTo(msg, buf, sizeof(buf), 1) == 0);
    msg.success = true;
    assert(shm::ProtoCodec<Message>::EncodeTo(msg, buf, 13, 1) == 0);
    assert(shm::ProtoCodec<Message>::EncodeTo(msg, buf, 14, 1) == 14);
}
template <size_t Capacity>
void TestProtoRollback()
{
    using Channel = shm::RingChannel<Capacity>;
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    shm::UniqueFd a{sockets[0]}, b{sockets[1]};
    Channel client, server;
    std::thread peer([&] {
        assert(Channel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &server) == shm::ErrorCode::kOk);
    });
    assert(Channel::CreateRingChannel(a.Get(), shm::ChannelRole::kClient, &client) == shm::ErrorCode::kOk);
    peer.join();
    Message msg;
    // 推进到尾部，使非镜像的序列化走回退缓冲区。
    if (!client.WriteMirrored())
    {
        char buf[512]{};
        assert(client.TryWrite(buf, 512) == 0);
        assert(server.TryRead(buf, 512) == 512);
        assert(client.TryWrite(buf, 500) == 0);
        assert(server.TryRead(buf, 500) == 500);
    }
    const auto pos = server.WritePos();
    msg.size = 16; msg.success = false;
    assert(shm::ProtoCodec<Message>::Send(client, msg, 1) == -1);
    assert(server.WritePos() == pos);
    msg.size = 0;
    assert(shm::ProtoCodec<Message>::Send(client, msg, 1) == -1);
    assert(server.WritePos() == pos);
    typename Channel::ChannelBatchWriter batch;
    assert(client.StartBatch(&batch) == shm::ErrorCode::kOk);
    const char previous = 'p';
    assert(batch.TryWrite(&previous, 1) == 0);
    assert(shm::ProtoCodec<Message>::template SendBatch<Capacity>(batch, msg, 1) == -1);
    assert(batch.PendingBytes() == 1 && batch.Count() == 1);
    batch.Flush();
    assert(server.Readable() == 1);
    char received;
    assert(server.TryRead(&received, 1) == 1 && received == previous);
}
void TestPathOwnership()
{
    const auto path = Path("owner");
    shm::EventLoop loop;
    shm::ShmServer first(&loop, path), second(&loop, path);
    Configure(first); Configure(second);
    assert(first.Start());
    assert(!second.Start() && second.LastSysErrno() == EADDRINUSE);
    first.Stop();
    assert(second.Start());
    second.Stop();
    // 替换路径后，旧服务不得删除新服务的入口。
    assert(first.Start());
    assert(::unlink(path.c_str()) == 0);
    assert(second.Start());
    first.Stop();
    shm::UniqueFd client;
    assert(shm::ConnectUnixSocket(path, &client) == shm::ErrorCode::kOk);
    second.Stop();
    // 已有普通文件也不得删除。
    shm::UniqueFd file{::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)};
    assert(file);
    assert(!first.Start());
    struct stat st{};
    assert(::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    assert(::unlink(path.c_str()) == 0);
}
void TestStartException()
{
    const auto path = Path("start");
    shm::EventLoop loop;
    shm::ShmServer server(&loop, path);
    Configure(server);
    int failures = 0;
    server.SetStartFailedCallback([&](const auto&) { ++failures; });
    server.SetStartSuccessCallback([] { throw std::runtime_error("startup"); });
    assert(!server.Start() && failures == 1 && server.LastError() == shm::ErrorCode::kSystem);
    assert(::access(path.c_str(), F_OK) < 0 && errno == ENOENT);
    server.SetStartSuccessCallback([] { throw std::bad_alloc{}; });
    assert(!server.Start() && failures == 2 && server.LastError() == shm::ErrorCode::kNoMemory);
    server.SetStartSuccessCallback([] {});
    assert(server.Start() && server.LastError() == shm::ErrorCode::kOk && server.LastSysErrno() == 0);
    server.Stop();
}
void TestVerifyException()
{
    const auto path = Path("verify");
    shm::UniqueFd listener;
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    std::thread peer([&] {
        shm::UniqueFd fd{::accept4(listener.Get(), nullptr, nullptr, SOCK_CLOEXEC)};
        shm::DefaultRingChannel channel;
        assert(shm::DefaultRingChannel::CreateRingChannel(fd.Get(), shm::ChannelRole::kServer, &channel) == shm::ErrorCode::kOk);
        char b;
        (void)::read(fd.Get(), &b, 1);
    });
    shm::EventLoop loop;
    shm::ShmAsyncClient client(&loop);
    bool fired = false;
    client.SetCodecFactory([] { return std::make_shared<comm::codec::Codec>(); });
    client.SetConnectionCallback([](const auto&) {});
    client.SetMessageCallback([](const auto&, const auto&) {});
    client.SetCloseCallback([](const auto&) {});
    client.SetVerifyCallback([&](const auto& conn) {
        conn->StartVerifyTimer(1, 0, [&](int64_t) { fired = true; });
        throw std::bad_alloc{};
    });
    client.Connect(path);
    assert(!client.Conn() && client.LastError() == shm::ErrorCode::kNoMemory);
    peer.join();
    int timer;
    assert(loop.AddOneShotTimer(20, [&](int, short) { loop.Stop(); }, &timer) == shm::ErrorCode::kOk);
    assert(loop.Run(-1) == shm::ErrorCode::kOk && !fired);
    ::unlink(path.c_str());
}
void TestCloexec()
{
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    shm::UniqueFd a{sockets[0]}, b{sockets[1]}, event;
    assert(shm::CreateEventfd(&event) == shm::ErrorCode::kOk);
    assert(shm::SendFd(a.Get(), event.Get(), shm::FdTag::kEventfd) == shm::ErrorCode::kOk);
    shm::TaggedFd received;
    assert(shm::RecvFd(b.Get(), &received) == shm::ErrorCode::kOk);
    assert(::fcntl(received.fd.Get(), F_GETFD) & FD_CLOEXEC);
    const auto path = Path("cloexec");
    shm::UniqueFd listener, client;
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    assert(shm::ConnectUnixSocket(path, &client) == shm::ErrorCode::kOk);
    assert((::fcntl(listener.Get(), F_GETFD) & FD_CLOEXEC) && (::fcntl(client.Get(), F_GETFD) & FD_CLOEXEC));
    ::unlink(path.c_str());
}
}  // namespace
int main()
{
    TestTimerIdentity(); TestStopWakeup(); TestProtoSizes(); TestProtoRollback<1024>(); TestProtoRollback<4096>();
    TestPathOwnership(); TestStartException(); TestVerifyException(); TestCloexec();
    std::puts("=== SECOND AUDIT REGRESSION PASSED ===");
}
