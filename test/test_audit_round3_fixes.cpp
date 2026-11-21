#include <shm_ipc/shm_server.hpp>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "contract_test.hpp"

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
struct ContractCodec : comm::codec::Codec
{
    shm::ShmConnection* conn;
    int mode;
    ContractCodec(shm::ShmConnection* c, int m) : conn(c), mode(m) {}
    void Mutate(int action)
    {
        if (action == 1) conn->ResizeBuffer(1024 * 1024, 1024 * 1024);
        if (action == 2) conn->SetCodec(nullptr);
        if (action == 3) conn->Close();
        if (action == 4) (void)conn->SendCmd(1);
        if (action == 5) (void)conn->DrainRing();
        if (action == 6) conn->SetMessageCallback({});
    }
    int32_t EncodedSize(const std::any&) const override
    {
        if (mode == 7) conn->SetCodec(nullptr);
        return mode == 8 ? 256 : -1;
    }
    int32_t Encode(const std::any&, comm::byte* buf, size_t capacity, size_t* size) override
    {
        if (mode >= 1 && mode <= 6) Mutate(mode);
        if (mode == 8) Mutate(1);  // 直写ring路径也禁止重入扩容
        if (mode == 30 && capacity < 256 * 1024) { *size = 256 * 1024; return 0; }
        buf[0] = 42;
        return 1;
    }
    int32_t Decode(const comm::byte*, size_t, std::any& pkg, size_t* size) override
    {
        if (mode >= 11 && mode <= 16) Mutate(mode - 10);
        if ((mode >= 21 && mode <= 26) || mode == 31) { *size = 1; return comm::kCodecSkipPackage; }
        pkg = 42;
        return 1;
    }
    int32_t SendKeepAlive(comm::byte* buf, size_t capacity, size_t* size) override
    {
        if (mode >= 21 && mode <= 26) Mutate(mode - 20);
        if (mode == 31 && capacity < 768 * 1024) { *size = 768 * 1024; return 0; }
        buf[0] = 7;
        return 1;
    }
};
void ContractViolation(int mode)
{
    Pair pair; shm::EventLoop loop;
    auto conn = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(pair.a), std::move(pair.client));
    conn->SetCodec(std::make_shared<ContractCodec>(conn.get(), mode));
    conn->SetKeepAlive(true); conn->OnConnection();
    if (mode <= 8) (void)conn->SendCmd(1);
    else
    {
        char byte = 1; assert(pair.server.TryWrite(&byte, 1) == 0);
        (void)conn->DrainRing();
    }
}
void CallbackViolation(int mode)
{
    shm::EventLoop loop;
    auto conn = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
    if (mode == 0)
    {
        conn->OnConnection();
        conn->SetMessageCallback([&](const auto&, const auto&) { conn->SetMessageCallback({}); });
        conn->OnMessage(1);
    }
    if (mode == 1)
    {
        conn->SetConnectionCallback([&](const auto&) { conn->SetConnectionCallback({}); });
        conn->OnConnection();
    }
    if (mode == 2)
    {
        conn->SetVerifyCallback([&](const auto&) { conn->SetVerifyCallback({}); });
        conn->OnConnection();
    }
    if (mode == 3)
    {
        conn->OnConnection();
        conn->SetCloseCallback([&](const auto&) { conn->SetCloseCallback({}); });
        conn->OnClose();
    }
    if (mode == 4)
    {
        conn->OnConnection();
        conn->SetMessageCallback([&](const auto&, const auto&) { (void)conn->DrainRing(); });
        Pair pair;
        auto reader = std::make_shared<shm::ShmConnection>(&loop, 2, std::move(pair.a), std::move(pair.client));
        reader->SetCodec(std::make_shared<ContractCodec>(reader.get(), 0)); reader->OnConnection();
        reader->SetMessageCallback([&](const auto&, const auto&) { (void)reader->DrainRing(); });
        char byte = 1; assert(pair.server.TryWrite(&byte, 1) == 0); (void)reader->DrainRing();
    }
}
void PureCodecAndOuterManagement()
{
    Pair pair; shm::EventLoop loop;
    auto conn = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(pair.a), std::move(pair.client));
    conn->SetCodec(std::make_shared<ContractCodec>(conn.get(), 30)); conn->OnConnection();
    assert(conn->SendCmd(1));  // codec返回所需容量，传输层在虚函数返回后扩容
    conn->ResizeBuffer(512 * 1024, 1024 * 1024);
    assert(conn->SendCmd(2));
    char response[2]; assert(pair.server.ReadExact(response, 2) == 0);
    assert(response[0] == 42 && response[1] == 42);
    conn->SetCodec(std::make_shared<ContractCodec>(conn.get(), 31)); conn->SetKeepAlive(true);
    char bytes[2] = {1, 1}; assert(pair.server.TryWrite(bytes, 2) == 0);
    assert(conn->DrainRing() == 0); assert(pair.server.ReadExact(response, 2) == 0);
    assert(response[0] == 7 && response[1] == 7);
    conn->SetCodec(std::make_shared<ContractCodec>(conn.get(), 0));
    int delivered = 0;
    conn->SetMessageCallback([&](const auto& active, const auto&) {
        assert(active->SendCmd(1));  // 消息回调中的正常回复
        ++delivered;
    });
    assert(pair.server.TryWrite(bytes, 2) == 0); assert(conn->DrainRing() == 2 && delivered == 2);
    assert(pair.server.ReadExact(response, 2) == 0);
    conn->SetMessageCallback([&](const auto& active, const auto&) { active->Close(); });
    assert(pair.server.TryWrite(bytes, 2) == 0); assert(conn->DrainRing() == 1);
    assert(conn->State() == comm::TcpConnStates::closeing);
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
    for (int mode : {1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25, 26})
        ExpectContractAbort([=] { ContractViolation(mode); });
    for (int mode = 0; mode < 5; ++mode) ExpectContractAbort([=] { CallbackViolation(mode); });
    PureCodecAndOuterManagement();
    ListenRollback();
    puts("round3 fixes passed");
}
