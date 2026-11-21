#include <shm_ipc/shm_client.hpp>
#include <shm_ipc/shm_connection.hpp>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

// 确保最后一批数据在读方完成空环检查后发布，且仅由 EOF 唤醒。
std::atomic<bool> reader_polling{false};
thread_local bool observe_poll = false;
extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout)
{
    if (observe_poll) reader_polling.store(true, std::memory_order_release);
    return __real_poll(fds, count, timeout);
}

void TestEof(bool peek)
{
    const std::string path = "/tmp/shm-eof-reentry-" + std::to_string(getpid());
    shm::UniqueFd listener;
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    reader_polling.store(false);
    std::thread peer([&] {
        shm::UniqueFd sock{::accept(listener.Get(), nullptr, nullptr)};
        assert(sock);
        shm::DefaultRingChannel ch;
        assert(shm::DefaultRingChannel::CreateRingChannel(sock.Get(), shm::ChannelRole::kServer, &ch) == shm::ErrorCode::kOk);
        while (!reader_polling.load(std::memory_order_acquire)) std::this_thread::yield();
        assert(ch.TryWrite("abcdefghij", 10) == 0);
        assert(::shutdown(sock.Get(), SHUT_RDWR) == 0);
    });
    shm::ShmClient client;
    assert(client.Connect(path));
    observe_poll = true;
    std::string result;
    if (peek)
    {
        const void *s1, *s2; uint32_t n1, n2;
        assert(client.PeekMessage(&s1, &n1, &s2, &n2, 1000) == 10);
        assert(n1 >= 3);
        result.append(static_cast<const char*>(s1), 3);
        client.CommitMessage(3);
    }
    char buf[3];
    for (;;)
    {
        int n = client.ReadMessage(buf, sizeof(buf), 1000);
        assert(n >= 0);
        if (!n) break;
        result.append(buf, n);
    }
    assert(result == "abcdefghij");
    assert(!client.Connected());
    assert(client.ReadMessage(buf, sizeof(buf), 0) == 0);
    const void *s1, *s2; uint32_t n1, n2;
    assert(client.PeekMessage(&s1, &n1, &s2, &n2, 0) == 0);
    client.Close();
    assert(client.ReadMessage(buf, sizeof(buf), 0) == -1);
    observe_poll = false;
    peer.join();
    assert(shm::UnlinkUnixSocket(path));
}

struct Pair
{
    shm::UniqueFd a, b;
    shm::DefaultRingChannel client, server;
    Pair()
    {
        int fds[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        a.Reset(fds[0]); b.Reset(fds[1]);
        std::thread peer([&] {
            assert(shm::DefaultRingChannel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &server) == shm::ErrorCode::kOk);
        });
        assert(shm::DefaultRingChannel::CreateRingChannel(a.Get(), shm::ChannelRole::kClient, &client) == shm::ErrorCode::kOk);
        peer.join();
    }
};
struct ByteCodec : comm::codec::Codec
{
    int32_t Decode(const comm::byte* data, size_t n, std::any& pkg, size_t*) override
    {
        if (!n) return 0;
        pkg = static_cast<char>(*data);
        return 1;
    }
};
void TestReentry()
{
    Pair p; shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
    c->SetCodec(std::make_shared<ByteCodec>()); c->OnConnection();
    std::string received;
    c->SetMessageCallback([&](const auto&, const std::any& pkg) {
        received += std::any_cast<char>(pkg);
        assert(c->DrainRing() == 0);
        assert(c->DrainRing(true) == 0);
    });
    assert(p.server.TryWrite("abc", 3) == 0);
    assert(c->DrainRing() == 3);
    assert(received == "abc" && c->Channel().Readable() == 0);
    assert(p.server.TryWrite("d", 1) == 0);
    assert(c->DrainRing() == 1 && received == "abcd");
}
struct PrefixCodec : comm::codec::Codec
{
    bool consumed = false;
    int32_t Decode(const comm::byte*, size_t n, std::any& pkg, size_t*) override
    {
        if (!consumed && n) { consumed = true; pkg = 'x'; return 1; }
        return 0;
    }
};
void TestFullIncomplete()
{
    Pair p; shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
    c->SetCodec(std::make_shared<comm::codec::Codec>()); c->OnConnection();
    std::vector<char> bytes(shm::DefaultRingChannel::kMaxWriteSize, 'x');
    assert(p.server.TryWrite(bytes.data(), bytes.size()) == 0);
    assert(c->DrainRing() == 0); // 未满的残帧仍可等待续写
    assert(p.server.TryWrite(bytes.data(), bytes.size()) == 0);
    assert(c->DrainRing() == -1);
    assert(c->Channel().Readable() == shm::DefaultRingBuf::kCapacity);
    // 再次调用仍正确报错，保护标志已恢复。
    assert(c->DrainRing() == -1);
    c->SetCodec(std::make_shared<PrefixCodec>());
    assert(c->DrainRing() == 1); // 可消费前缀必须先释放，不能按初始满环误报
    assert(c->Channel().Readable() == shm::DefaultRingBuf::kCapacity - 1);
}
int main()
{
    TestEof(false); TestEof(true); TestReentry(); TestFullIncomplete();
    std::puts("=== EOF / REENTRY REGRESSIONS PASSED ===");
}
