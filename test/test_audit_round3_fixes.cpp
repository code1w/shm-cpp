#include <shm_ipc/shm_server.hpp>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>

struct Pair
{
    shm::UniqueFd a, b;
    shm::DefaultRingChannel client, server;
    Pair()
    {
        int s[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
        a.Reset(s[0]); b.Reset(s[1]);
        std::thread t([&] {
            assert(shm::DefaultRingChannel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &server) == shm::ErrorCode::kOk);
        });
        assert(shm::DefaultRingChannel::CreateRingChannel(a.Get(), shm::ChannelRole::kClient, &client) == shm::ErrorCode::kOk);
        t.join();
    }
};
struct ReentrantCodec : comm::codec::Codec
{
    shm::ShmConnection* conn;
    int mode;
    bool* destroyed;
    int value = 42;
    ReentrantCodec(shm::ShmConnection* c, int m, bool* d) : conn(c), mode(m), destroyed(d) {}
    ~ReentrantCodec() override { *destroyed = true; }
    int32_t Decode(const comm::byte*, size_t, std::any& pkg, size_t* size) override
    {
        if (mode <= 2)
        {
            if (mode == 0) conn->SetCodec(std::make_shared<comm::codec::Codec>());
            if (mode == 1) conn->SetCodec(nullptr);
            if (mode == 2) conn->Close();
            assert(!*destroyed);
            pkg = value;
            return 1;
        }
        *size = 1;
        return comm::kCodecSkipPackage;
    }
    int32_t Encode(const std::any&, comm::byte* buf, size_t, size_t*) override
    {
        buf[0] = 99;
        return 1;
    }
    int32_t SendKeepAlive(comm::byte* buf, size_t, size_t*) override
    {
        conn->ResizeBuffer(1024 * 1024, 1024 * 1024);
        assert(!conn->SendCmd(std::any{}));  // 同一借用作用域禁止发送重入
        if (mode == 4) conn->Close();
        if (mode == 5) conn->SetCodec(nullptr);
        if (mode == 6) conn->SetCodec(std::make_shared<comm::codec::Codec>());
        assert(!*destroyed);
        buf[0] = static_cast<comm::byte>(value);
        if (mode == 7) throw std::runtime_error("heartbeat");
        return 1;
    }
};
void CodecReentry(int mode)
{
    Pair pair;
    shm::EventLoop loop;
    bool destroyed = false;
    auto conn = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(pair.a), std::move(pair.client));
    conn->SetCodec(std::make_shared<ReentrantCodec>(conn.get(), mode, &destroyed));
    conn->SetKeepAlive(true);
    int delivered = 0;
    conn->SetMessageCallback([&](const auto&, const auto&) { ++delivered; });
    conn->OnConnection();
    char bytes[2] = {};
    assert(pair.server.TryWrite(bytes, sizeof(bytes)) == 0);
    bool threw = false;
    int result = 0;
    try { result = conn->DrainRing(); }
    catch (const std::runtime_error&) { threw = true; }
    assert(delivered == 0);
    assert(threw == (mode == 7));
    if (mode == 0 || mode == 1 || mode == 5 || mode == 6) assert(result == -1 && destroyed);
    if (mode == 3)
    {
        assert(pair.server.Readable() == 2);
        char response[2]; assert(pair.server.ReadExact(response, 2) == 0);
        assert(response[0] == 42 && response[1] == 42);
        assert(conn->SendCmd(std::any{}));  // 重入保护已解除，正常消息仍能发送
        assert(pair.server.ReadExact(response, 1) == 0 && response[0] == 99);
    }
    else
    {
        assert(pair.server.Readable() == 0);
        assert(conn->State() == comm::TcpConnStates::closeing);
    }
    conn.reset();
    assert(destroyed);
}

static int listen_failure = 0;
static std::string replacement_path;
static shm::UniqueFd replacement_fd;
extern "C" int __real_listen(int, int);
extern "C" int __wrap_listen(int fd, int backlog)
{
    if (listen_failure)
    {
        const int mode = listen_failure;
        listen_failure = 0;
        if (mode == 2)
        {
            assert(unlink(replacement_path.c_str()) == 0);
            // 在原节点仍存活时创建另一个 socket，避免 inode 复用。
            assert(shm::ListenUnixSocket(replacement_path, &replacement_fd) == shm::ErrorCode::kOk);
        }
        errno = ENOMEM;
        return -1;
    }
    return __real_listen(fd, backlog);
}
void ListenRollback()
{
    shm::EventLoop loop;
    const std::string path = "/tmp/shm-round3-" + std::to_string(getpid());
    shm::ShmServer server(&loop, path);
    server.SetConnectionCallback([](const auto&) {});
    server.SetMessageCallback([](const auto&, const auto&) {});
    server.SetCloseCallback([](const auto&) {});
    server.SetCodecFactory([] { return std::make_shared<comm::codec::Codec>(); });
    listen_failure = 1;
    assert(!server.Start() && server.LastSysErrno() == ENOMEM);
    assert(access(path.c_str(), F_OK) < 0 && errno == ENOENT);
    assert(server.Start());
    server.Stop();
    replacement_path = path;
    listen_failure = 2;
    shm::UniqueFd out;
    int err = 0;
    assert(shm::ListenUnixSocket(path, &out, 16, &err) != shm::ErrorCode::kOk);
    assert(err == ENOMEM && !out);
    assert(access(path.c_str(), F_OK) == 0);  // 不能删除重入创建的其他服务入口
    shm::UniqueFd peer;
    assert(shm::ConnectUnixSocket(path, &peer) == shm::ErrorCode::kOk);
    replacement_fd.Reset();
    assert(unlink(path.c_str()) == 0);
}
int main()
{
    for (int mode = 0; mode < 8; ++mode) CodecReentry(mode);
    ListenRollback();
    puts("round3 fixes passed");
}
