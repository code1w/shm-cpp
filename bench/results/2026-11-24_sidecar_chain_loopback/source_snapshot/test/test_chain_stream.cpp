#include "../bench/chain_stream.hpp"
#include <cstdio>

namespace {
using namespace chain_bench;
using E = shm::ErrorCode;
struct Fixture
{
    shm::EventLoop loop;
    Stream source, sink;
    std::unique_ptr<unsigned char[]> bytes;
    uint64_t received = 0, queued = 0;
    E error     = E::kOk;
    bool ending = false;
    static E Packet(void* p, const Frame& frame)
    {
        auto* f = static_cast<Fixture*>(p);
        if (Sequence(frame) != f->received || !Verify(frame, kMaxBody, true))
            return E::kProtocol;
        ++f->received;
        if (f->queued < 12)
        {
            const auto ec = f->Queue();
            if (ec != E::kOk)
                return ec;
        }
        if (f->received == 12)
            f->loop.Stop();
        return E::kOk;
    }
    static E Unexpected(void*, const Frame&) { return E::kProtocol; }
    static void Closed(void* p, E ec)
    {
        auto* f = static_cast<Fixture*>(p);
        if (!f->ending)
        {
            f->error = ec;
            f->loop.Stop();
        }
    }
    E Queue()
    {
        WriteSequence(bytes.get(), queued);
        Frame frame{bytes.get(), kHeader + kMaxBody};
        const auto ec = source.Send(frame);
        if (ec == E::kOk)
            ++queued;
        return ec;
    }
    ~Fixture()
    {
        ending = true;
        source.Close();
        sink.Close();
    }
};
int Transfer()
{
    int fd[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) < 0)
        return 1;
    shm::UniqueFd left{fd[0]}, right{fd[1]};
    Fixture f;
    f.bytes.reset(new (std::nothrow) unsigned char[kHeader + kMaxBody]);
    if (!f.bytes)
        return 2;
    Write32(f.bytes.get(), kMagic);
    Write32(f.bytes.get() + 4, kMaxBody);
    for (uint32_t i = 0; i < kMaxBody; ++i)
        f.bytes[kHeader + i] = static_cast<unsigned char>(i * 131u + 17u);
    if (f.source.Init(&f.loop, std::move(left), false, 0, &f, Fixture::Unexpected, Fixture::Closed) != E::kOk)
        return 3;
    if (f.sink.Init(&f.loop, std::move(right), false, 0, &f, Fixture::Packet, Fixture::Closed) != E::kOk)
        return 4;
    int small = 4096;
    if (::setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) < 0)
        return 5;
    for (unsigned i = 0; i < 7; ++i)
        if (f.Queue() != E::kOk)
            return 6;
    if (f.Queue() != E::kTimeout)
        return 7;
    int timer;
    if (f.loop.AddOneShotTimer(
            5000,
            [&](int, short) {
                f.error = E::kTimeout;
                f.loop.Stop();
            },
            &timer) != E::kOk)
        return 8;
    const auto ec = f.loop.Run(100);
    f.ending      = true;
    if (ec != E::kOk || f.error != E::kOk || f.received != 12 || f.queued != 12)
        return 9;
    if (f.source.Stats().tx_bytes != uint64_t(12) * (kHeader + kMaxBody) || !f.source.Stats().eagain)
        return 10;
    return 0;
}
struct Truncated
{
    shm::EventLoop loop;
    Stream sink;
    E error     = E::kOk;
    bool ending = false;
    static E Packet(void*, const Frame&) { return E::kProtocol; }
    static void Closed(void* p, E ec)
    {
        auto* f = static_cast<Truncated*>(p);
        if (!f->ending)
        {
            f->error = ec;
            f->loop.Stop();
        }
    }
    ~Truncated()
    {
        ending = true;
        sink.Close();
    }
};
int PartialEof()
{
    int fd[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) < 0)
        return 1;
    shm::UniqueFd writer{fd[0]}, reader{fd[1]};
    Truncated f;
    if (f.sink.Init(&f.loop, std::move(reader), false, 0, &f, Truncated::Packet, Truncated::Closed) != E::kOk)
        return 2;
    unsigned char bytes[8]{};
    Write32(bytes, kMagic);
    Write32(bytes + 4, 16);
    if (::send(writer.Get(), bytes, 8, MSG_NOSIGNAL) != 8 || ::shutdown(writer.Get(), SHUT_WR) < 0)
        return 3;
    int timer;
    if (f.loop.AddOneShotTimer(
            1000,
            [&](int, short) {
                f.error = E::kTimeout;
                f.loop.Stop();
            },
            &timer) != E::kOk)
        return 4;
    if (f.loop.Run(100) != E::kOk || f.error != E::kProtocol)
        return 5;
    return 0;
}
}  // namespace
int main()
{
    const auto a = Transfer(), b = PartialEof();
    if (a || b)
    {
        std::fprintf(stderr, "FAIL chain stream transfer=%d EOF=%d\n", a, b);
        return 1;
    }
    std::puts("PASS: chain stream partial writes, EAGAIN, queue wrap/full and truncated EOF");
    return 0;
}
