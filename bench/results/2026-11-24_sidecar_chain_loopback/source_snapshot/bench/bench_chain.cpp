// Gate -> local IPC -> sidecar A -> TCP -> sidecar B -> local IPC -> World.
#include "chain_local.hpp"
#include "random_batch.hpp"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <limits>

namespace chain_bench {
using E                                  = shm::ErrorCode;
static volatile sig_atomic_t interrupted = 0;
static void Signal(int) { interrupted = 1; }
static uint64_t Now() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static bool Sample(uint64_t seq, uint32_t stride) noexcept
{
    if (stride == 1)
        return true;
    uint64_t x = seq + UINT64_C(0x9e3779b97f4a7c15);
    x          = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x          = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return (stride & (stride - 1)) ? x % stride == 0 : (x & (stride - 1)) == 0;
}
struct Histogram
{
    std::array<uint64_t, 4096> bins{};
    uint64_t samples = 0, max = 0;
    void Add(uint64_t n) noexcept
    {
        if (!n)
            n = 1;
        const unsigned e    = 63 - __builtin_clzll(n);
        const uint64_t base = uint64_t(1) << e;
        const unsigned sub  = unsigned((n - base) * 64 / base);
        ++bins[e * 64 + sub];
        ++samples;
        if (n > max)
            max = n;
    }
    uint64_t Percentile(double fraction) const noexcept
    {
        const auto rank = uint64_t(std::ceil(double(samples) * fraction));
        uint64_t sum    = 0;
        for (unsigned i = 0; i < bins.size(); ++i)
            if ((sum += bins[i]) >= rank && rank)
            {
                const auto e    = i / 64;
                const auto base = uint64_t(1) << e;
                return base + ((i % 64 + 1) * base + 63) / 64;
            }
        return 0;
    }
};
enum class Role
{
    Gate,
    GateSidecar,
    WorldSidecar,
    World
};
struct Options
{
    Role role               = Role::Gate;
    const char* actor       = "gate";
    const char* local       = nullptr;
    const char* mode        = "uds";
    const char* tcp_address = "127.0.0.1";
    uint16_t port           = 19001;
    int tcp_buffer          = 0;
    uint32_t bytes = 16, low = 1, high = 64, stride = 64, control_interval = 256;
    uint64_t seed = 1, messages = 0;
    double seconds = 5, warmup = 1, timeout = 30;
};
static bool Integer(const char* s, uint64_t max, uint64_t* out) noexcept
{
    if (!s || !*s || *s == '-')
        return false;
    char* end    = nullptr;
    errno        = 0;
    const auto n = std::strtoull(s, &end, 10);
    if (errno || *end || n > max)
        return false;
    *out = n;
    return true;
}
static E Parse(int argc, char** argv, Options* o) noexcept
{
    if (argc < 2)
        return E::kInvalidArgument;
    o->actor = argv[1];
    if (std::strcmp(argv[1], "gate") == 0)
        o->role = Role::Gate;
    else if (std::strcmp(argv[1], "gate-sidecar") == 0)
        o->role = Role::GateSidecar;
    else if (std::strcmp(argv[1], "world-sidecar") == 0)
        o->role = Role::WorldSidecar;
    else if (std::strcmp(argv[1], "world") == 0)
        o->role = Role::World;
    else
        return E::kInvalidArgument;
    for (int i = 2; i < argc; i += 2)
    {
        if (i + 1 >= argc)
            return E::kInvalidArgument;
        const char* key = argv[i];
        const char* v   = argv[i + 1];
        uint64_t n      = 0;
        if (std::strcmp(key, "--local") == 0)
            o->local = v;
        else if (std::strcmp(key, "--mode") == 0)
            o->mode = v;
        else if (std::strcmp(key, "--tcp-address") == 0)
            o->tcp_address = v;
        else if (std::strcmp(key, "--seconds") == 0 || std::strcmp(key, "--warmup") == 0 ||
                 std::strcmp(key, "--timeout") == 0)
        {
            char* end      = nullptr;
            const double d = std::strtod(v, &end);
            if (*end || !(d >= 0 && d <= 86400))
                return E::kInvalidArgument;
            if (std::strcmp(key, "--seconds") == 0)
                o->seconds = d;
            else if (std::strcmp(key, "--warmup") == 0)
                o->warmup = d;
            else
                o->timeout = d;
        }
        else
        {
            if (!Integer(v, std::numeric_limits<uint64_t>::max(), &n))
                return E::kInvalidArgument;
            if (std::strcmp(key, "--bytes") == 0 && n <= kMaxBody)
                o->bytes = uint32_t(n);
            else if (std::strcmp(key, "--batch-min") == 0 && n <= 256)
                o->low = uint32_t(n);
            else if (std::strcmp(key, "--batch-max") == 0 && n <= 256)
                o->high = uint32_t(n);
            else if (std::strcmp(key, "--stride") == 0 && n <= 65536)
                o->stride = uint32_t(n);
            else if (std::strcmp(key, "--seed") == 0)
                o->seed = n;
            else if (std::strcmp(key, "--messages") == 0)
                o->messages = n;
            else if (std::strcmp(key, "--tcp-port") == 0 && n <= 65535)
                o->port = uint16_t(n);
            else if (std::strcmp(key, "--tcp-buffer") == 0 && n <= INT32_MAX)
                o->tcp_buffer = int(n);
            else if (std::strcmp(key, "--control-interval") == 0 && n <= 65536)
                o->control_interval = uint32_t(n);
            else
                return E::kInvalidArgument;
        }
    }
    if (!o->local || std::strlen(o->local) >= sizeof(sockaddr_un{}.sun_path) || o->bytes < 16 || !o->low ||
        o->low > o->high || !o->stride || !o->control_interval || o->seconds <= 0 || o->timeout <= 0)
        return E::kInvalidArgument;
    if (std::strcmp(o->mode, "uds") && std::strcmp(o->mode, "shm-eventfd") && std::strcmp(o->mode, "shm-busy"))
        return E::kInvalidArgument;
    if (o->role == Role::Gate && uint64_t(o->bytes) * o->high > 2 * 1024 * 1024)
        return E::kInvalidArgument;
    if (o->role == Role::GateSidecar && o->port == 0)
        return E::kInvalidArgument;
    return E::kOk;
}
struct Result
{
    bool completed = false;
    E error        = E::kOk;
    uint64_t sent = 0, received = 0, measured = 0, local_rx = 0, local_tx = 0, net_rx = 0;
    StreamStats net;
    Histogram latency;
    double seconds   = 0;
    uint64_t batches = 0, batch_sum = 0, prefix_count = 0, prefix_hash = UINT64_C(14695981039346656037);
};
class Engine
{
    const Options& o_;
    const uint32_t magic_;
    shm::EventLoop loop_;
    LocalLink local_;
    Stream tcp_;
    shm::UniqueFd listener_;
    acceptance_bench::RandomBatch schedule_;
    std::unique_ptr<unsigned char[]> request_;
    Frame request_view_;
    std::array<uint64_t, 256> stamps_{};
    Result result_;
    uint64_t phase_end_ = 0, phase_start_ = 0, last_progress_ = 0;
    bool measuring_ = false, ending_ = false;
    uint16_t actual_port_ = 0;
    uint32_t Magic() const noexcept { return magic_; }
    void Fail(E ec)
    {
        if (result_.error == E::kOk)
            result_.error = ec;
        ending_ = true;
        loop_.Stop();
    }
    E SendLocal(const Frame& f)
    {
        const auto ec = local_.Send(f);
        if (ec == E::kOk)
            ++result_.local_tx;
        else
            Fail(ec);
        return ec;
    }
    E SendTcp(const Frame& f)
    {
        const auto ec = tcp_.Send(f);
        if (ec != E::kOk)
            Fail(ec);
        return ec;
    }
    E SendBatch()
    {
        uint64_t amount = schedule_.Next();
        if (measuring_ && o_.messages)
            amount = std::min<uint64_t>(amount, o_.messages - result_.measured);
        if (measuring_)
        {
            ++result_.batches;
            result_.batch_sum += amount;
            if (result_.prefix_count < 64)
            {
                result_.prefix_hash = (result_.prefix_hash ^ amount) * UINT64_C(1099511628211);
                ++result_.prefix_count;
            }
        }
        for (uint64_t i = 0; i < amount; ++i)
        {
            WriteSequence(request_.get(), result_.sent);
            if (measuring_ && Sample(result_.sent, o_.stride))
                stamps_[result_.sent % o_.high] = Now();
            const auto ec = SendLocal(request_view_);
            if (ec != E::kOk)
                return ec;
            ++result_.sent;
        }
        return E::kOk;
    }
    E OnLocal(const Frame& frame)
    {
        if (ending_)
            return result_.error;
        if (Read32(frame.data) != Magic())
        {
            Fail(E::kProtocol);
            return E::kProtocol;
        }
        const auto arrival = o_.role == Role::Gate && measuring_ && Sample(result_.received, o_.stride) ? Now() : 0;
        ++result_.local_rx;
        if (o_.role == Role::GateSidecar || o_.role == Role::WorldSidecar)
            return SendTcp(frame);
        if (o_.role == Role::World)
        {
            const auto seq  = Sequence(frame);
            const auto body = frame.bytes - kHeader;
            if (seq != result_.local_rx - 1 || !Verify(frame, body, Sample(seq, 64)))
            {
                Fail(E::kProtocol);
                return E::kProtocol;
            }
            return SendLocal(frame);
        }
        const auto seq = Sequence(frame);
        if (seq != result_.received || !Verify(frame, o_.bytes, Sample(seq, 64)))
        {
            Fail(E::kProtocol);
            return E::kProtocol;
        }
        if (measuring_)
        {
            ++result_.measured;
            if (arrival)
                result_.latency.Add(arrival - stamps_[result_.received % o_.high]);
        }
        ++result_.received;
        if (result_.received != result_.sent)
        {
            if (arrival)
                last_progress_ = arrival;
            return E::kOk;
        }
        const auto now = Now();
        last_progress_ = now;
        if (measuring_ && (o_.messages ? result_.measured >= o_.messages : now >= phase_end_))
        {
            result_.seconds   = double(now - phase_start_) / 1e9;
            result_.completed = true;
            ending_           = true;
            loop_.Stop();
            return E::kOk;
        }
        if (!measuring_ && now >= phase_end_)
        {
            measuring_   = true;
            phase_start_ = now;
            phase_end_   = now + uint64_t(o_.seconds * 1e9);
            schedule_.Reset(o_.seed);
        }
        return SendBatch();
    }
    E OnTcp(const Frame& frame)
    {
        if (ending_)
            return result_.error;
        if (Read32(frame.data) != Magic())
        {
            Fail(E::kProtocol);
            return E::kProtocol;
        }
        ++result_.net_rx;
        return SendLocal(frame);
    }
    void OnClosed(E ec, bool network)
    {
        if (ending_)
            return;
        const bool normal =
            (ec == E::kPeerClosed) && ((o_.role == Role::GateSidecar && !network && result_.local_rx > 0) ||
                                       (o_.role == Role::WorldSidecar && network && result_.net_rx > 0) ||
                                       (o_.role == Role::World && !network && result_.local_rx > 0));
        if (normal)
        {
            ending_ = true;
            loop_.Stop();
        }
        else
            Fail(ec);
    }
    void Connected()
    {
        if (o_.role != Role::Gate)
            return;
        measuring_   = o_.warmup == 0;
        phase_start_ = last_progress_ = Now();
        phase_end_                    = phase_start_ + uint64_t((measuring_ ? o_.seconds : o_.warmup) * 1e9);
        schedule_.Reset(measuring_ ? o_.seed : o_.seed ^ UINT64_C(0xd1b54a32d192ed03));
        const auto ec = SendBatch();
        if (ec != E::kOk)
            Fail(ec);
    }
    static E LocalPacket(void* self, const Frame& f) { return static_cast<Engine*>(self)->OnLocal(f); }
    static E TcpPacket(void* self, const Frame& f) { return static_cast<Engine*>(self)->OnTcp(f); }
    static void LocalClosed(void* self, E ec) { static_cast<Engine*>(self)->OnClosed(ec, false); }
    static void TcpClosed(void* self, E ec) { static_cast<Engine*>(self)->OnClosed(ec, true); }
    static void LocalConnected(void* self) { static_cast<Engine*>(self)->Connected(); }
    void AcceptTcp()
    {
        shm::UniqueFd fd{::accept4(listener_.Get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
        if (!fd)
        {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                Fail(shm::ErrnoToErrorCode(errno));
            return;
        }
        const auto ec = tcp_.Init(&loop_, std::move(fd), true, o_.tcp_buffer, this, TcpPacket, TcpClosed);
        if (ec != E::kOk)
        {
            Fail(ec);
            return;
        }
        loop_.RemoveFd(listener_.Get());
        listener_.Reset();
    }

 public:
    explicit Engine(const Options& options)
        : o_(options),
          magic_(kMagic + (std::strcmp(options.mode, "uds") == 0           ? 0u
                           : std::strcmp(options.mode, "shm-eventfd") == 0 ? 1u
                                                                           : 2u)),
          schedule_(o_.low, o_.high, o_.seed)
    {}
    ~Engine() { Stop(); }
    E Init()
    {
        auto ec = loop_.SetBusyPolling(std::strcmp(o_.mode, "shm-busy") == 0, o_.control_interval);
        if (ec != E::kOk)
            return ec;
        if (o_.role == Role::Gate)
        {
            request_.reset(new (std::nothrow) unsigned char[o_.bytes + kHeader]);
            if (!request_)
                return E::kNoMemory;
            Write32(request_.get(), Magic());
            Write32(request_.get() + 4, o_.bytes);
            for (uint32_t i = 0; i < o_.bytes; ++i)
                request_[kHeader + i] = static_cast<unsigned char>(i * 131u + 17u);
            request_view_ = {request_.get(), o_.bytes + kHeader};
        }
        if (o_.role == Role::GateSidecar)
        {
            shm::UniqueFd fd;
            ec = ConnectTcp(o_.tcp_address, o_.port, &fd);
            if (ec != E::kOk)
                return ec;
            ec = tcp_.Init(&loop_, std::move(fd), true, o_.tcp_buffer, this, TcpPacket, TcpClosed);
            if (ec != E::kOk)
                return ec;
        }
        const bool server = o_.role == Role::World || o_.role == Role::GateSidecar;
        ec = local_.Init(&loop_, o_.local, server, std::strcmp(o_.mode, "uds") == 0, this, LocalPacket, LocalClosed,
                         LocalConnected);
        if (ec != E::kOk)
            return ec;
        if (o_.role == Role::WorldSidecar)
        {
            ec = ListenTcp(o_.tcp_address, o_.port, &listener_, &actual_port_);
            if (ec != E::kOk)
                return ec;
            loop_.AddFd(listener_.Get(), [this](int, short) { AcceptTcp(); });
        }
        int timer = -1;
        ec        = loop_.AddTimer(
            250,
            [this](int fd, short) {
                shm::EventLoop::DrainTimerfd(fd);
                if (interrupted)
                {
                    Fail(E::kPeerClosed);
                    return;
                }
                if (o_.role == Role::Gate && last_progress_ && Now() - last_progress_ > uint64_t(o_.timeout * 1e9))
                    Fail(E::kTimeout);
            },
            &timer);
        if (ec != E::kOk)
            return ec;
        std::printf("{\"role\":\"ready\",\"actor\":\"%s\",\"mode\":\"%s\",\"tcp_port\":%u,\"control_interval\":%u}\n",
                    o_.actor, o_.mode, actual_port_, o_.control_interval);
        std::fflush(stdout);
        return result_.error;
    }
    E Run()
    {
        const auto ec = loop_.Run(100);
        if (ec != E::kOk)
            Fail(ec);
        return result_.error;
    }
    void Stop()
    {
        ending_ = true;
        local_.Close();
        tcp_.Close();
        if (listener_)
        {
            loop_.RemoveFd(listener_.Get());
            listener_.Reset();
        }
    }
    Result Summary() const
    {
        Result r = result_;
        r.net    = tcp_.Stats();
        return r;
    }
};
static uint64_t Fds() noexcept
{
    DIR* d = ::opendir("/proc/self/fd");
    if (!d)
        return UINT64_MAX;
    uint64_t n = 0;
    while (auto* e = ::readdir(d))
        if (e->d_name[0] != '.')
            ++n;
    ::closedir(d);
    return n;
}
static void Print(const Options& o, const Result& r, uint64_t base, uint64_t final)
{
    if (o.role == Role::Gate && r.completed)
    {
        const auto rpc     = double(r.measured) / r.seconds;
        const auto payload = rpc * o.bytes / 1048576.0;
        std::printf(
            "{\"role\":\"result\",\"bytes\":%u,\"batch_min\":%u,\"batch_max\":%u,\"seed\":%llu,\"messages\":%llu,\"seconds\":%.9f,\"rpc_s\":%.3f,\"payload_mib_s\":%.6f,\"duplex_payload_mib_s\":%.6f,\"duplex_tcp_framed_mib_s\":%.6f,\"samples\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"max_ns\":%llu,\"batches\":%llu,\"batch_messages\":%llu,\"prefix_count\":%llu,\"prefix_hash\":%llu}\n",
            o.bytes, o.low, o.high, (unsigned long long)o.seed, (unsigned long long)r.measured, r.seconds, rpc, payload,
            2 * payload, 2 * rpc * (o.bytes + kHeader) / 1048576.0, (unsigned long long)r.latency.samples,
            (unsigned long long)r.latency.Percentile(.5), (unsigned long long)r.latency.Percentile(.99),
            (unsigned long long)r.latency.Percentile(.999), (unsigned long long)r.latency.max,
            (unsigned long long)r.batches, (unsigned long long)r.batch_sum, (unsigned long long)r.prefix_count,
            (unsigned long long)r.prefix_hash);
    }
    std::printf(
        "{\"role\":\"chain_final\",\"actor\":\"%s\",\"mode\":\"%s\",\"completed\":%s,\"error\":\"%s\",\"base_fds\":%llu,\"final_fds\":%llu,\"sent\":%llu,\"received\":%llu,\"local_rx\":%llu,\"local_tx\":%llu,\"tcp_rx_frames\":%llu,\"tcp_tx_frames\":%llu,\"tcp_tx_bytes\":%llu,\"tcp_rx_bytes\":%llu,\"tcp_send_calls\":%llu,\"tcp_eagain\":%llu}\n",
        o.actor, o.mode, r.completed ? "true" : "false", shm::ErrorCodeName(r.error), (unsigned long long)base,
        (unsigned long long) final, (unsigned long long)r.sent, (unsigned long long)r.received,
        (unsigned long long)r.local_rx, (unsigned long long)r.local_tx, (unsigned long long)r.net_rx,
        (unsigned long long)r.net.queued_frames, (unsigned long long)r.net.tx_bytes, (unsigned long long)r.net.rx_bytes,
        (unsigned long long)r.net.send_calls, (unsigned long long)r.net.eagain);
}
}  // namespace chain_bench
int main(int argc, char** argv)
{
    using namespace chain_bench;
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, Signal);
    ::signal(SIGTERM, Signal);
    Options options;
    auto ec = Parse(argc, argv, &options);
    if (ec != E::kOk)
    {
        std::fputs(
            "Usage: bench_chain gate|gate-sidecar|world-sidecar|world --local PATH --mode uds|shm-eventfd|shm-busy [--tcp-address IPv4 --tcp-port PORT] [--bytes N --batch-min N --batch-max N --seed N --seconds N --warmup N --messages N --stride N --timeout N --tcp-buffer N]\n",
            stderr);
        return 2;
    }
    const auto base = Fds();
    Result result;
    {
        Engine engine(options);
        ec = engine.Init();
        if (ec == E::kOk)
            ec = engine.Run();
        engine.Stop();
        result = engine.Summary();
    }
    const auto final = Fds();
    if (base == UINT64_MAX || base != final)
        ec = E::kSystem;
    if (ec != E::kOk)
        result.error = ec;
    Print(options, result, base, final);
    if (ec != E::kOk || (options.role == Role::Gate && !result.completed))
        return 2;
    return 0;
}
