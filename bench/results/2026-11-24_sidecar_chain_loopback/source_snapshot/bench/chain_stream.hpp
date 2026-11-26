#pragma once
#include "chain_wire.hpp"
#include <shm_ipc/common.hpp>
#include <shm_ipc/event_loop.hpp>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <memory>
#include <algorithm>

namespace chain_bench {
inline shm::ErrorCode Nonblocking(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return shm::ErrnoToErrorCode(errno);
    return shm::ErrorCode::kOk;
}
inline shm::ErrorCode TcpAddress(const char* ip, uint16_t port, sockaddr_in* out) noexcept
{
    *out            = {};
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);
    return ::inet_pton(AF_INET, ip, &out->sin_addr) == 1 ? shm::ErrorCode::kOk : shm::ErrorCode::kInvalidArgument;
}
inline shm::ErrorCode ListenTcp(const char* ip, uint16_t port, shm::UniqueFd* out, uint16_t* actual) noexcept
{
    sockaddr_in address{};
    auto ec = TcpAddress(ip, port, &address);
    if (ec != shm::ErrorCode::kOk)
        return ec;
    shm::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!fd)
        return shm::ErrnoToErrorCode(errno);
    int one = 1;
    if (::setsockopt(fd.Get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
        ::bind(fd.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd.Get(), 1) < 0)
        return shm::ErrnoToErrorCode(errno);
    socklen_t size = sizeof(address);
    if (::getsockname(fd.Get(), reinterpret_cast<sockaddr*>(&address), &size) < 0)
        return shm::ErrnoToErrorCode(errno);
    *actual = ntohs(address.sin_port);
    *out    = std::move(fd);
    return shm::ErrorCode::kOk;
}
inline shm::ErrorCode ConnectTcp(const char* ip, uint16_t port, shm::UniqueFd* out) noexcept
{
    sockaddr_in address{};
    auto ec = TcpAddress(ip, port, &address);
    if (ec != shm::ErrorCode::kOk)
        return ec;
    shm::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    if (!fd)
        return shm::ErrnoToErrorCode(errno);
    if (::connect(fd.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
    {
        if (errno != EINPROGRESS)
            return shm::ErrnoToErrorCode(errno);
        pollfd p{fd.Get(), POLLOUT, 0};
        int n;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                    .count();
            if (remaining <= 0)
                return shm::ErrorCode::kTimeout;
            n = ::poll(&p, 1, static_cast<int>(remaining));
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            return n == 0 ? shm::ErrorCode::kTimeout : shm::ErrnoToErrorCode(errno);
        int error     = 0;
        socklen_t len = sizeof(error);
        if (::getsockopt(fd.Get(), SOL_SOCKET, SO_ERROR, &error, &len) < 0)
            return shm::ErrnoToErrorCode(errno);
        if (error)
            return shm::ErrnoToErrorCode(error);
    }
    *out = std::move(fd);
    return shm::ErrorCode::kOk;
}
struct StreamStats
{
    uint64_t queued_frames = 0, received_frames = 0, tx_bytes = 0, rx_bytes = 0, send_calls = 0, recv_calls = 0,
             eagain = 0;
};
// Fixed queues; only partial frames are compacted when the RX tail fills.
class Stream
{
    shm::EventLoop* loop_ = nullptr;
    shm::UniqueFd fd_;
    std::unique_ptr<unsigned char[]> tx_, rx_;
    uint32_t tx_head_ = 0, tx_tail_ = 0, tx_size_ = 0, rx_begin_ = 0, rx_end_ = 0;
    short mask_ = POLLIN;
    bool open_ = false, busy_tcp_ = false;
    shm::EventLoop::HookId hook_ = 0;
    void* context_               = nullptr;
    PacketHandler packet_        = nullptr;
    CloseHandler close_          = nullptr;
    StreamStats stats_;
    void Register()
    {
        loop_->AddFd(
            fd_.Get(), [this](int, short events) { Io(events); }, mask_);
    }
    void Watch(short mask)
    {
        if (mask == mask_)
            return;
        mask_ = mask;
        if (!busy_tcp_)
        {
            loop_->RemoveFd(fd_.Get());
            Register();
        }
    }
    void Io(short events)
    {
        if (!open_)
            return;
        if (events & (POLLIN | POLLHUP | POLLERR))
            Receive();
        if (open_ && (events & POLLOUT))
            Flush();
        if (open_ && (events & POLLNVAL))
            Close(shm::ErrorCode::kSystem);
    }
    void Receive()
    {
        while (open_)
        {
            if (rx_end_ == kQueueBytes)
            {
                if (!rx_begin_)
                {
                    Close(shm::ErrorCode::kProtocol);
                    return;
                }
                const auto pending = rx_end_ - rx_begin_;
                std::memmove(rx_.get(), rx_.get() + rx_begin_, pending);
                rx_begin_ = 0;
                rx_end_   = pending;
            }
            ++stats_.recv_calls;
            const auto n = ::recv(fd_.Get(), rx_.get() + rx_end_, kQueueBytes - rx_end_, 0);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                Close(shm::ErrnoToErrorCode(errno));
                return;
            }
            if (n == 0)
            {
                Close(rx_begin_ != rx_end_ ? shm::ErrorCode::kProtocol : shm::ErrorCode::kPeerClosed);
                return;
            }
            rx_end_ += uint32_t(n);
            stats_.rx_bytes += uint64_t(n);
            while (open_ && rx_begin_ < rx_end_)
            {
                const auto used = Probe(rx_.get() + rx_begin_, rx_end_ - rx_begin_);
                if (used < 0)
                {
                    Close(shm::ErrorCode::kProtocol);
                    return;
                }
                if (!used)
                    break;
                const Frame frame{rx_.get() + rx_begin_, uint32_t(used)};
                ++stats_.received_frames;
                const auto ec = packet_(context_, frame);
                if (ec != shm::ErrorCode::kOk)
                {
                    Close(ec);
                    return;
                }
                rx_begin_ += uint32_t(used);
            }
            if (rx_begin_ == rx_end_)
                rx_begin_ = rx_end_ = 0;
        }
    }

 public:
    Stream()                         = default;
    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream()
    {
        Close();
        if (loop_ && hook_)
            loop_->RemovePostDispatchHook(hook_);
    }
    shm::ErrorCode Init(shm::EventLoop* loop, shm::UniqueFd fd, bool tcp, int tcp_buffer, void* context,
                        PacketHandler packet, CloseHandler closed)
    {
        if (open_ || !fd || !packet || !closed)
            return shm::ErrorCode::kInvalidArgument;
        auto ec = Nonblocking(fd.Get());
        if (ec != shm::ErrorCode::kOk)
            return ec;
        if (tcp)
        {
            int one = 1;
            if (::setsockopt(fd.Get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
                return shm::ErrnoToErrorCode(errno);
        }
        const int buffer = tcp ? tcp_buffer : 4 * 1024 * 1024;
        if (buffer > 0 && (::setsockopt(fd.Get(), SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) < 0 ||
                           ::setsockopt(fd.Get(), SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) < 0))
            return shm::ErrnoToErrorCode(errno);
        tx_.reset(new (std::nothrow) unsigned char[kQueueBytes]);
        rx_.reset(new (std::nothrow) unsigned char[kQueueBytes]);
        if (!tx_ || !rx_)
            return shm::ErrorCode::kNoMemory;
        // Match Ring::Init's first-touch policy; setup/page faults stay before warmup.
        std::memset(tx_.get(), 0, kQueueBytes);
        std::memset(rx_.get(), 0, kQueueBytes);
        loop_    = loop;
        fd_      = std::move(fd);
        context_ = context;
        packet_  = packet;
        close_   = closed;
        open_    = true;
        ec       = loop_->AddPostDispatchHook([this] { Flush(); }, &hook_);
        if (ec != shm::ErrorCode::kOk)
        {
            open_ = false;
            fd_.Reset();
            return ec;
        }
        busy_tcp_ = tcp && loop_->BusyPolling();
        if (busy_tcp_)
        {
            ec = loop_->AddBusyPollFd(fd_.Get(), [this](int, short) {
                if (!open_)
                    return shm::ErrorCode::kOk;
                pollfd p{fd_.Get(), mask_, 0};
                const int n = ::poll(&p, 1, 0);
                if (n < 0 && errno != EINTR)
                {
                    Close(shm::ErrnoToErrorCode(errno));
                    return shm::ErrorCode::kOk;
                }
                if (n > 0)
                    Io(p.revents);
                return shm::ErrorCode::kOk;
            });
            if (ec != shm::ErrorCode::kOk)
            {
                open_ = false;
                fd_.Reset();
                return ec;
            }
        }
        else
            Register();
        int snd = 0, rcv = 0;
        socklen_t n = sizeof(int);
        ::getsockopt(fd_.Get(), SOL_SOCKET, SO_SNDBUF, &snd, &n);
        ::getsockopt(fd_.Get(), SOL_SOCKET, SO_RCVBUF, &rcv, &n);
        std::printf(
            "{\"role\":\"stream_config\",\"tcp\":%s,\"busy_tcp_poll\":%s,\"send_buffer\":%d,\"receive_buffer\":%d,\"nodelay\":%s}\n",
            tcp ? "true" : "false", busy_tcp_ ? "true" : "false", snd, rcv, tcp ? "true" : "false");
        return shm::ErrorCode::kOk;
    }
    shm::ErrorCode Send(const Frame& frame)
    {
        if (!open_)
            return shm::ErrorCode::kPeerClosed;
        if (!frame.data || frame.bytes < kHeader || frame.bytes > kMaxBody + kHeader)
            return shm::ErrorCode::kInvalidArgument;
        if (frame.bytes > kQueueBytes - tx_size_)
            return shm::ErrorCode::kTimeout;
        const auto first = std::min(frame.bytes, kQueueBytes - tx_tail_);
        std::memcpy(tx_.get() + tx_tail_, frame.data, first);
        if (first < frame.bytes)
            std::memcpy(tx_.get(), frame.data + first, frame.bytes - first);
        tx_tail_ = (tx_tail_ + frame.bytes) % kQueueBytes;
        tx_size_ += frame.bytes;
        ++stats_.queued_frames;
        Watch(POLLIN | POLLOUT);
        return shm::ErrorCode::kOk;
    }
    void Flush()
    {
        while (open_ && tx_size_)
        {
            const auto size = std::min(tx_size_, kQueueBytes - tx_head_);
            ++stats_.send_calls;
            const auto n = ::send(fd_.Get(), tx_.get() + tx_head_, size, MSG_NOSIGNAL);
            if (n > 0)
            {
                tx_head_ = (tx_head_ + uint32_t(n)) % kQueueBytes;
                tx_size_ -= uint32_t(n);
                stats_.tx_bytes += uint64_t(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                ++stats_.eagain;
                Watch(POLLIN | POLLOUT);
                return;
            }
            Close(n == 0 ? shm::ErrorCode::kPeerClosed : shm::ErrnoToErrorCode(errno));
            return;
        }
        if (open_)
            Watch(POLLIN);
    }
    void Close(shm::ErrorCode reason = shm::ErrorCode::kPeerClosed)
    {
        if (!open_)
            return;
        open_ = false;
        loop_->RemoveFd(fd_.Get());
        fd_.Reset();
        close_(context_, reason);  // keep hook alive until object destruction outside dispatch
    }
    bool Open() const noexcept { return open_; }
    const StreamStats& Stats() const noexcept { return stats_; }
};
}  // namespace chain_bench
