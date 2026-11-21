#include <shm_ipc/shm_connection.hpp>
#include <shm_ipc/frame_reader.hpp>
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/shm_client.hpp>
#include <shm_ipc/shm_server.hpp>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

// 强制镜像预留失败，覆盖生产环境中 mmap 回退到普通映射的路径。
std::atomic<bool> fail_mirror{false};
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (fail_mirror.load() && prot == PROT_NONE && (flags & MAP_ANONYMOUS))
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(addr, len, prot, flags, fd, off);
}
struct Pair
{
    shm::UniqueFd a, b;
    shm::DefaultRingChannel client, server;
    Pair()
    {
        int s[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
        a.Reset(s[0]); b.Reset(s[1]);
        std::thread peer([&] {
            assert(shm::DefaultRingChannel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &server) == shm::ErrorCode::kOk);
        });
        assert(shm::DefaultRingChannel::CreateRingChannel(a.Get(), shm::ChannelRole::kClient, &client) == shm::ErrorCode::kOk);
        peer.join();
    }
};
void TestCorruptPositions()
{
    using R = shm::RingBuf<1024>;
    shm::MmapRegion m;
    shm::UniqueFd fd;
    assert(shm::CreateMemfd("corrupt", R::kShmSize, &fd) == shm::ErrorCode::kOk);
    assert(m.Map(fd.Get(), R::kShmSize, PROT_READ|PROT_WRITE, MAP_SHARED) == shm::ErrorCode::kOk);
    R::Init(m.Get());
    auto* h = static_cast<shm::RingHeader*>(m.Get());
    char out[4096];
    for (uint64_t w : {uint64_t{4096}, UINT64_MAX})
    {
        h->write_pos.store(w); h->read_pos.store(0);
        const void *a = nullptr, *b = nullptr; uint32_t na=0, nb=0;
        assert(R::Peek(m.Get(), &a, &na, &b, &nb) == -2);
        assert(R::PeekContig(m.Get(), &a, &na) == -2);
        assert(R::TryRead(m.Get(), out, sizeof(out)) == 0);
        assert(R::TryReadContig(m.Get(), out, sizeof(out)) == 0);
        assert(R::ReadExact(m.Get(), out, 1) == -1);
        assert(R::ReadExactContig(m.Get(), out, 1) == -1);
        R::CommitRead(m.Get(), 1); assert(h->read_pos.load() == 0);
        assert(R::FreeSpace(m.Get()) == 0 && R::Available(m.Get()) == 0);
        assert(R::TryWrite(m.Get(), out, 1) == -1);
        assert(R::TryWriteContig(m.Get(), out, 1) == -1);
        R::BatchWriter batch(m.Get());
        assert(batch.FreeBytes() == 0 && batch.Reserve(1) == nullptr && batch.TryWrite(out, 1) == -1);
    }
    // 正常 uint64 游标回绕仍然有效。
    h->write_pos.store(UINT64_MAX - 3); h->read_pos.store(UINT64_MAX - 3);
    const char bytes[] = "abcdefgh";
    assert(R::TryWrite(m.Get(), bytes, 8) == 0);
    assert(R::ReadExact(m.Get(), out, 8) == 0 && std::memcmp(out, bytes, 8) == 0);
}
void TestCorruptConnection()
{
    Pair p;
    const char byte = 0;
    assert(p.server.TryWrite(&byte, 1) == 0);
    const void *data = nullptr, *second = nullptr;
    uint32_t len = 0, second_len = 0;
    assert(p.client.Peek(&data, &len, &second, &second_len) == 0);
    auto* hdr = reinterpret_cast<shm::RingHeader*>(
        const_cast<char*>(static_cast<const char*>(data)) - shm::kRingDataOffset);
    hdr->write_pos.store(shm::DefaultRingBuf::kCapacity + 1);
    shm::FrameReader<> reader;
    assert(reader.TryRecv(p.client, &data, &len) == -3);
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
    c->SetCodec(std::make_shared<comm::codec::Codec>());
    c->OnConnection();
    assert(c->DrainRing() == -1);
}
struct LengthCodec : comm::codec::Codec
{
    int32_t result = 0;
    int32_t Encode(const std::any&, comm::byte* b, size_t n, size_t*) override
    { std::memset(b, 'x', n); return result ? result : static_cast<int32_t>(n+1); }
    int32_t SendKeepAlive(comm::byte* b, size_t n, size_t*) override
    { std::memset(b, 'x', n); return static_cast<int32_t>(n+1); }
    int32_t Decode(const comm::byte*, size_t n, std::any&, size_t* size) override
    { if (!n) return 0; *size=1; return comm::kCodecSkipPackage; }
};
void TestLengthsAndClose()
{
    Pair p; shm::EventLoop loop;
    auto c=std::make_shared<shm::ShmConnection>(&loop,1,std::move(p.a),std::move(p.client));
    auto codec=std::make_shared<LengthCodec>(); c->SetCodec(codec); c->OnConnection();
    assert(!c->SendCmd(1) && p.server.Readable()==0);
    codec->result=-99; assert(!c->SendCmd(1) && p.server.Readable()==0);
    c->SetKeepAlive(true); char heartbeat=1;
    assert(p.server.TryWrite(&heartbeat,1)==0);
    assert(c->DrainRing()==0 && p.server.Readable()==0);
    int closed=0; c->SetCloseCallback([&](const auto&) { ++closed; });
    c->OnClose(); c->OnClose();
    char byte; assert(::recv(p.b.Get(),&byte,1,MSG_DONTWAIT)==0 && closed==1);
    assert(!c->SendCmd(1)); // shared_ptr 仍持有对象，关闭已对对端可见
}
void TestNotifyValidation()
{
    shm::UniqueFd good, blocking{::eventfd(0,EFD_CLOEXEC)}, fake;
    assert(shm::CreateEventfd(&good)==shm::ErrorCode::kOk);
    assert(shm::ValidateNotifyEventfd(good.Get())==shm::ErrorCode::kOk);
    assert(shm::ValidateNotifyEventfd(blocking.Get())==shm::ErrorCode::kProtocol);
    assert(shm::CreateMemfd("fake",8,&fake)==shm::ErrorCode::kOk);
    assert(::fcntl(fake.Get(),F_SETFL,O_NONBLOCK)==0);
    assert(shm::ValidateNotifyEventfd(fake.Get())==shm::ErrorCode::kProtocol);
    int s[2]; assert(::socketpair(AF_UNIX,SOCK_STREAM,0,s)==0);
    shm::UniqueFd a{s[0]},b{s[1]},ring; shm::MmapRegion m;
    assert(shm::CreateMemfd("ring",shm::DefaultRingBuf::kShmSize,&ring)==shm::ErrorCode::kOk);
    assert(m.Map(ring.Get(),shm::DefaultRingBuf::kShmSize,PROT_READ|PROT_WRITE,MAP_SHARED)==shm::ErrorCode::kOk);
    shm::DefaultRingBuf::Init(m.Get());
    std::thread liar([&] {
        shm::TaggedFd d;
        assert(shm::RecvFd(b.Get(),&d)==shm::ErrorCode::kOk);
        assert(shm::RecvFd(b.Get(),&d)==shm::ErrorCode::kOk);
        assert(shm::SendFd(b.Get(),ring.Get(),shm::FdTag::kMemfd)==shm::ErrorCode::kOk);
        assert(shm::SendFd(b.Get(),fake.Get(),shm::FdTag::kEventfd)==shm::ErrorCode::kOk);
    });
    shm::DefaultRingChannel ch;
    assert(shm::DefaultRingChannel::CreateRingChannel(a.Get(),shm::ChannelRole::kClient,&ch)==shm::ErrorCode::kProtocol);
    liar.join();
}
struct SequenceCodec : comm::codec::Codec
{
    int next=1, calls=0;
    int32_t Decode(const comm::byte* b,size_t n,std::any& pkg,size_t*) override
    {
        if (!n) return 0;
        ++calls;
        if (*b!=next++) return comm::kCodecDisconnect;
        pkg=int(*b); return 1;
    }
};
void TestStatefulWrap(bool mirrored)
{
    fail_mirror.store(!mirrored); Pair p; fail_mirror.store(false);
    if (!mirrored) assert(!p.client.ReadMirrored() && !p.server.WriteMirrored());
    // 空环游标推进到倒数一个字节：两条消息分别位于尾部与头部。
    std::vector<char> padding(shm::DefaultRingBuf::kMaxWriteSize,0);
    for (size_t len : {padding.size(),padding.size()-1})
    {
        assert(p.server.TryWrite(padding.data(),len)==0);
        assert(p.client.TryRead(padding.data(),len)==len);
    }
    char bytes[2]={1,2}; assert(p.server.TryWrite(bytes,2)==0);
    shm::EventLoop loop;
    auto c=std::make_shared<shm::ShmConnection>(&loop,1,std::move(p.a),std::move(p.client));
    auto codec=std::make_shared<SequenceCodec>(); c->SetCodec(codec); c->OnConnection();
    std::vector<int> received;
    c->SetMessageCallback([&](const auto&,const auto& pkg){ received.push_back(std::any_cast<int>(pkg)); });
    assert(c->DrainRing()==2 && codec->calls==2 && (received==std::vector<int>{1,2}));
}

struct DirectCodec : comm::codec::Codec
{
    int32_t EncodedSize(const std::any&) const override { return 256; }
    int32_t Encode(const std::any&, comm::byte* b, size_t n, size_t* size) override
    {
        if (n < 256) { *size = 256; return 0; }
        std::memset(b, 1, 256); return 256;
    }
};
void TestDirectFallback()
{
    fail_mirror = true; Pair p; fail_mirror = false;
    assert(!p.client.WriteMirrored());
    std::vector<char> buf(shm::DefaultRingBuf::kMaxWriteSize);
    for (size_t n : {buf.size(), buf.size() - 128})
    {
        assert(p.client.TryWrite(buf.data(), n) == 0);
        assert(p.server.TryRead(buf.data(), n) == n);
    }
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
    c->SetCodec(std::make_shared<DirectCodec>()); c->OnConnection();
    assert(c->SendCmd(1));
    assert(p.server.TryRead(buf.data(), 256) == 256);
    for (size_t i = 0; i < 256; ++i) assert(buf[i] == 1);
}
void TestMessageException()
{
    for (bool allocation_failure : {false, true})
    {
        Pair p; char bytes[2] = {1, 2}; assert(p.server.TryWrite(bytes, 2) == 0);
        shm::EventLoop loop;
        auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
        auto codec = std::make_shared<SequenceCodec>();
        c->SetCodec(codec); c->OnConnection(); int count = 0;
        c->SetMessageCallback([&](const auto&, const auto&) {
            ++count;
            if (allocation_failure) throw std::bad_alloc{};
            throw std::runtime_error("message callback");
        });
        bool caught = false;
        try { c->DrainRing(); } catch (const std::exception&) { caught = true; }
        assert(caught && count == 1 && c->Channel().ReadPos() == 1);
        assert(c->State() == comm::TcpConnStates::closeing);
        assert(c->DrainRing() == 0 && count == 1 && codec->calls == 1);
        char b; assert(::recv(p.b.Get(), &b, 1, MSG_DONTWAIT) == 0);
    }
}
void TestCloseException()
{
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
    c->OnConnection(); c->SetContext(c); c->SetRpcContext(c);
    std::weak_ptr<shm::ShmConnection> weak = c;
    c->SetCloseCallback([](const auto&) { throw std::runtime_error("close callback"); });
    bool caught = false;
    try { c->OnClose(); } catch (const std::runtime_error&) { caught = true; }
    assert(caught && !c->Context().has_value() && !c->RpcContext().has_value());
    c->OnClose(); c.reset(); assert(weak.expired());
}
void TestLongVerifyTimeout()
{
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
    c->OnConnection(); bool fired = false;
    c->StartVerifyTimer(int64_t{INT32_MAX} + 1, 0, [&](int64_t) { fired = true; });
    int tfd;
    assert(loop.AddOneShotTimer(5, [&](int, short) { loop.Stop(); }, &tfd) == shm::ErrorCode::kOk);
    assert(loop.Run(-1) == shm::ErrorCode::kOk && !fired);
    c->Close(); // 取消长定时器，不遗留 fd/回调
}
void TestFactoryRecovery()
{
    for (bool allocation_failure : {false, true})
    {
        const std::string path = "/tmp/shm-factory-recovery-" + std::to_string(::getpid());
        shm::UniqueFd listener;
        assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
        std::thread peer([&] {
            for (int i = 0; i < 2; ++i)
            {
                shm::UniqueFd sock{::accept(listener.Get(), nullptr, nullptr)};
                shm::DefaultRingChannel ch;
                assert(shm::DefaultRingChannel::CreateRingChannel(sock.Get(), shm::ChannelRole::kServer, &ch) == shm::ErrorCode::kOk);
                char b; (void)::read(sock.Get(), &b, 1);
            }
        });
        shm::EventLoop loop; shm::ShmAsyncClient client(&loop);
        client.SetConnectionCallback([](const auto&) {});
        client.SetCloseCallback([](const auto&) {});
        client.SetMessageCallback([](const auto&, const auto&) {});
        int calls = 0, errors = 0;
        client.SetConnectErrorCallback([&](bool, const auto&) { ++errors; });
        client.SetCodecFactory([&]() -> comm::codec::CodecPtr {
            if (++calls == 1)
            {
                if (allocation_failure) throw std::bad_alloc{};
                throw std::runtime_error("factory");
            }
            return std::make_shared<comm::codec::Codec>();
        });
        bool caught = false;
        try { client.Connect(path); } catch (const std::runtime_error&) { caught = true; }
        assert(client.State() == comm::TcpClientStates::close && !client.Conn());
        if (allocation_failure) assert(!caught && errors == 1 && client.LastError() == shm::ErrorCode::kNoMemory);
        else assert(caught);
        client.Connect(path); assert(calls == 2 && client.Conn());
        client.Close(); peer.join(); assert(::unlink(path.c_str()) == 0);
    }
}
void TestHandshakeDeadline()
{
    int s[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    shm::UniqueFd a{s[0]}, b{s[1]}, fd;
    assert(shm::CreateMemfd("slow-peer", shm::DefaultRingBuf::kShmSize, &fd) == shm::ErrorCode::kOk);
    shm::ErrorCode result = shm::ErrorCode::kOk; int sys_errno = 0;
    const auto start = std::chrono::steady_clock::now();
    std::thread server([&] {
        shm::DefaultRingChannel ch;
        result = shm::DefaultRingChannel::CreateRingChannel(b.Get(), shm::ChannelRole::kServer, &ch, &sys_errno);
    });
    char byte = 0; iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{}; msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    auto* c = CMSG_FIRSTHDR(&msg); c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    const int sent_fd = fd.Get(); std::memcpy(CMSG_DATA(c), &sent_fd, sizeof(sent_fd));
    assert(::sendmsg(a.Get(), &msg, MSG_NOSIGNAL) == 1);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    assert(::send(a.Get(), &byte, 1, MSG_NOSIGNAL) == 1);
    server.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(result == shm::ErrorCode::kTimeout && sys_errno == ETIMEDOUT);
    assert(elapsed >= std::chrono::milliseconds(4500) && elapsed < std::chrono::milliseconds(7500));
    // deadline 路径不改变调用方的 socket 超时配置。
    timeval timeout{}; socklen_t len = sizeof(timeout);
    assert(::getsockopt(b.Get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, &len) == 0);
    assert(timeout.tv_sec == 0 && timeout.tv_usec == 0);
}

struct ByteAnyCodec : comm::codec::Codec
{
    int32_t Decode(const comm::byte* data, size_t n, std::any& pkg, size_t*) override
    { if (!n) return 0; pkg = int(*data); return 1; }
};
void TestReentrantRollback()
{
    for (bool allocation_failure : {false, true})
    {
        std::string path = "/tmp/shm-reentry-recovery-" + std::to_string(getpid());
        shm::UniqueFd listener;
        assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
        std::thread peer([&] {
            for (int i = 0; i < 2; ++i)
            {
                shm::UniqueFd sock{::accept(listener.Get(), nullptr, nullptr)};
                shm::DefaultRingChannel ch;
                assert(shm::DefaultRingChannel::CreateRingChannel(sock.Get(), shm::ChannelRole::kServer, &ch) == shm::ErrorCode::kOk);
                char b; (void)::read(sock.Get(), &b, 1);
            }
        });
        shm::EventLoop loop; shm::ShmAsyncClient client(&loop);
        int opens = 0, closes = 0, errors = 0;
        client.SetCodecFactory([] { return std::make_shared<ByteAnyCodec>(); });
        client.SetMessageCallback([](const auto&, const auto&) {});
        client.SetConnectErrorCallback([&](bool, const auto&) { ++errors; });
        client.SetCloseCallback([&](const auto&) { if (++closes == 1) client.Connect(path); });
        client.SetConnectionCallback([&](const auto&) {
            if (++opens == 1)
            {
                client.Close();
                if (allocation_failure) throw std::bad_alloc{};
                throw std::runtime_error("old callback");
            }
        });
        bool caught = false;
        try { client.Connect(path); } catch (const std::runtime_error&) { caught = true; }
        assert(caught == !allocation_failure && opens == 2 && closes == 1 && errors == 0);
        assert(client.Conn() && client.State() == comm::TcpClientStates::established && client.LastError() == shm::ErrorCode::kOk);
        client.Close(); peer.join(); assert(::unlink(path.c_str()) == 0);
    }
}
void TestReaderOwnership()
{
    static_assert(!std::is_copy_constructible_v<shm::FrameReader<>>);
    static_assert(!std::is_copy_assignable_v<shm::FrameReader<>>);
    static_assert(std::is_nothrow_move_constructible_v<shm::FrameReader<>>);
    static_assert(!std::is_move_assignable_v<shm::FrameReader<>>);
    Pair p; char byte = 1;
    for (int i = 0; i < 2; ++i)
        assert(shm::SendFrame(p.server, 1, i, [&](auto& batch) { assert(batch.TryWrite(&byte, 1) == 0); }) == 0);
    shm::FrameReader<> reader; const void* payload; uint32_t len;
    assert(reader.TryRecv(p.client, &payload, &len) == 0);
    auto moved = std::move(reader);
    reader.Commit(p.client); assert(p.client.ReadPos() == 0);
    moved.Commit(p.client); assert(p.client.ReadPos() == 9 && p.client.Readable() == 9);
    assert(moved.TryRecv(p.client, &payload, &len) == 0 && moved.LastSeq() == 1);
    moved.Commit(p.client); assert(p.client.Readable() == 0);
}
void TestDrainScheduling()
{
    Pair p; shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
    c->SetCodec(std::make_shared<ByteAnyCodec>()); c->OnConnection();
    int count = 0, seen_at_timer = 0; char byte = 1;
    c->SetMessageCallback([&](const auto&, const auto&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (++count < 100) assert(p.server.TryWrite(&byte, 1) == 0);
        else loop.Stop();
    });
    loop.AddFd(c->NotifyFd(), [&](int fd, short) { shm::DefaultRingChannel::DrainNotify(fd); c->DrainRing(); });
    int timer;
    assert(loop.AddOneShotTimer(1, [&](int, short) { seen_at_timer = count; loop.Stop(); }, &timer) == shm::ErrorCode::kOk);
    assert(p.server.TryWrite(&byte, 1) == 0); p.server.NotifyPeer();
    assert(loop.Run(-1) == shm::ErrorCode::kOk && seen_at_timer > 0 && seen_at_timer < 100);
    assert(loop.Run(-1) == shm::ErrorCode::kOk && count == 100 && c->Channel().Readable() == 0);
    // 对端不再写，预加载数据也必须靠本地唤醒持续消费。
    count = 0; c->SetMessageCallback([&](const auto&, const auto&) { if (++count == 300) loop.Stop(); });
    std::vector<char> bytes(300, 1); assert(p.server.TryWrite(bytes.data(), bytes.size()) == 0);
    p.server.NotifyPeer(); assert(loop.Run(-1) == shm::ErrorCode::kOk && count == 300);
    count = 0; assert(p.server.TryWrite(bytes.data(), bytes.size()) == 0);
    assert(c->DrainRing(true) == 300 && count == 300 && c->Channel().Readable() == 0); // EOF 不截断尾部
}
void TestHeartbeatBudget()
{
    Pair p; shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, std::move(p.a), std::move(p.client));
    c->SetCodec(std::make_shared<LengthCodec>()); c->OnConnection();
    std::vector<char> bytes(300, 1); assert(p.server.TryWrite(bytes.data(), bytes.size()) == 0);
    assert(c->DrainRing() == 0 && c->Channel().Readable() >= 236);
    pollfd fd{c->NotifyFd(), POLLIN, 0}; assert(::poll(&fd, 1, 0) == 1 && (fd.revents & POLLIN));
    assert(c->DrainRing(true) == 0 && c->Channel().Readable() == 0);
}
void TestReadArguments()
{
    std::string path = "/tmp/shm-read-arguments-" + std::to_string(getpid());
    shm::UniqueFd listener;
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    std::thread peer([&] {
        shm::UniqueFd sock{::accept(listener.Get(), nullptr, nullptr)};
        shm::DefaultRingChannel ch;
        assert(shm::DefaultRingChannel::CreateRingChannel(sock.Get(), shm::ChannelRole::kServer, &ch) == shm::ErrorCode::kOk);
        char b = 1; assert(ch.TryWrite(&b, 1) == 0); ch.NotifyPeer(); (void)::read(sock.Get(), &b, 1);
    });
    shm::ShmClient client; assert(client.Connect(path));
    char buf[1]; assert(client.ReadMessage(nullptr, 1, -1) == -1 && client.LastError() == shm::ErrorCode::kInvalidArgument);
    assert(client.ReadMessage(buf, 0, -1) == 0 && client.ReadMessage(nullptr, 0, -1) == 0);
    assert(client.ReadMessage(buf, size_t{1} << 32, 100) == 1 && buf[0] == 1);
    client.Close(); peer.join(); assert(::unlink(path.c_str()) == 0);
}

void TestCallbackReplacement()
{
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
    c->OnConnection(); int next_calls = 0;
    auto token = std::make_shared<int>(1); std::weak_ptr<int> weak = token;
    c->SetMessageCallback([&, token, payload = std::string(1000, 'x')](const auto&, const auto&) {
        c->SetMessageCallback([&](const auto&, const auto&) { ++next_calls; });
        assert(!weak.expired() && *token == 1 && payload[0] == 'x');
    });
    token.reset(); c->OnMessage(1);
    assert(weak.expired() && next_calls == 0);
    c->OnMessage(1); assert(next_calls == 1);
    std::vector<int> observed;
    c->SetMessageCallback([&, count = 0](const auto&, const auto&) mutable { observed.push_back(++count); });
    c->OnMessage(1); c->OnMessage(1); assert((observed == std::vector<int>{1, 2}));

    auto verified = std::make_shared<shm::ShmConnection>(&loop, 2, shm::UniqueFd{}, shm::DefaultRingChannel{});
    verified->SetVerifyCallback([&, payload = std::string(1000, 'v')](const auto&) {
        verified->SetVerifyCallback({}); assert(payload[0] == 'v'); verified->OnVerify(true);
    });
    verified->SetConnectionCallback([&, payload = std::string(1000, 'c')](const auto&) {
        verified->SetConnectionCallback({}); assert(payload[0] == 'c');
    });
    verified->OnConnection(); assert(verified->State() == comm::TcpConnStates::established);
    verified->SetCloseCallback([&, payload = std::string(1000, 'q')](const auto&) {
        verified->SetCloseCallback({}); assert(payload[0] == 'q');
    });
    verified->OnClose();
}
void TestVerifyLifecycle()
{
    for (int action : {0, 1, 2, 3})
    {
        shm::EventLoop loop;
        auto c = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
        int opened = 0;
        c->SetConnectionCallback([&](const auto&) { ++opened; });
        c->SetVerifyCallback([&](const auto&) {
            if (action == 0) c->OnVerify(false);
            if (action == 1) c->Close();
            if (action == 2) c->OnClose();
            if (action == 3) c->OnVerify(true);
        });
        c->OnConnection(); assert(opened == (action == 3 ? 1 : 0));
    }
    // 异步验证尚未完成时，运输连接仍可触发一次建连事件。
    shm::EventLoop loop;
    auto c = std::make_shared<shm::ShmConnection>(&loop, 1, shm::UniqueFd{}, shm::DefaultRingChannel{});
    int opened = 0;
    c->SetVerifyCallback([](const auto&) {});
    c->SetConnectionCallback([&](const auto&) { ++opened; });
    c->OnConnection(); assert(opened == 1 && c->State() == comm::TcpConnStates::verifing);
}
void ConfigureCallbackServer(shm::ShmServer& server)
{
    server.SetConnectionCallback([](const auto&) {});
    server.SetMessageCallback([](const auto&, const auto&) {});
    server.SetCloseCallback([](const auto&) {});
    server.SetCodecFactory([] { return std::make_shared<ByteAnyCodec>(); });
}
void TestOwnerCallbackReplacement()
{
    shm::EventLoop loop;
    const std::string path = "/tmp/shm-owner-callback-" + std::to_string(getpid());
    shm::ShmServer server(&loop, path); ConfigureCallbackServer(server);
    int success = 0, failures = 0;
    server.SetStartSuccessCallback([&, payload = std::string(1000, 's')] {
        server.SetStartSuccessCallback({}); assert(payload[0] == 's'); ++success;
    });
    assert(server.Start() && success == 1);
    shm::ShmServer other(&loop, path); ConfigureCallbackServer(other);
    other.SetStartFailedCallback([&, payload = std::string(1000, 'f')](std::string) {
        other.SetStartFailedCallback({}); assert(payload[0] == 'f'); ++failures;
    });
    assert(!other.Start() && failures == 1); server.Stop();
    shm::ShmAsyncClient client(&loop);
    client.SetConnectionCallback([](const auto&) {});
    client.SetMessageCallback([](const auto&, const auto&) {});
    client.SetCloseCallback([](const auto&) {});
    client.SetCodecFactory([] { return std::make_shared<ByteAnyCodec>(); });
    int errors = 0;
    client.SetConnectErrorCallback([&, payload = std::string(1000, 'e')](bool, const std::string&) {
        client.SetConnectErrorCallback({}); assert(payload[0] == 'e'); ++errors;
    });
    client.Connect(path); assert(errors == 1 && !client.Conn());
    // 工厂返回值同样必须能在自替换后完成构造，且不复制 mutable 状态。
    shm::detail::StableCallback<comm::codec::CodecFactoryCallback> factory;
    factory = [&, payload = std::string(1000, 'r')] {
        factory = comm::codec::CodecFactoryCallback{};
        assert(payload[0] == 'r'); return std::make_shared<ByteAnyCodec>();
    };
    assert(factory() && !factory);
}
void TestNulPaths()
{
    const std::string path = "/tmp/shm-nul-rejected-" + std::to_string(getpid());
    const std::string invalid = path + std::string("\0suffix", 7);
    char out[108]; std::memset(out, 'x', sizeof(out));
    assert(!shm::CopyUnixPath(invalid, out, sizeof(out)) && out[0] == 'x');
    assert(!shm::CopyUnixPath(std::string_view("\0hidden", 7), out, sizeof(out)));
    shm::UniqueFd listener, connected; int sys_errno = 0;
    assert(shm::ListenUnixSocket(invalid, &listener, 16, &sys_errno) == shm::ErrorCode::kInvalidArgument && sys_errno == EINVAL);
    assert(::access(path.c_str(), F_OK) != 0);
    assert(shm::ListenUnixSocket(path, &listener) == shm::ErrorCode::kOk);
    sys_errno = 0;
    assert(shm::ConnectUnixSocket(invalid, &connected, &sys_errno) == shm::ErrorCode::kInvalidArgument && sys_errno == EINVAL);
    assert(!shm::UnlinkUnixSocket(invalid) && ::access(path.c_str(), F_OK) == 0);
    assert(shm::CopyUnixPath(path, out, sizeof(out)) && std::string(out) == path);
    assert(shm::ConnectUnixSocket(path, &connected) == shm::ErrorCode::kOk);
    assert(shm::UnlinkUnixSocket(path));
}
int main()
{
    TestCorruptPositions(); TestCorruptConnection(); TestNotifyValidation(); TestLengthsAndClose();
    TestStatefulWrap(false); TestStatefulWrap(true);
    TestDirectFallback(); TestMessageException(); TestCloseException();
    TestLongVerifyTimeout(); TestFactoryRecovery(); TestHandshakeDeadline();
    TestReentrantRollback(); TestReaderOwnership(); TestDrainScheduling(); TestHeartbeatBudget(); TestReadArguments();
    TestCallbackReplacement(); TestVerifyLifecycle(); TestOwnerCallbackReplacement(); TestNulPaths();
    std::puts("=== AUDIT HARDENING PASSED ===");
}
