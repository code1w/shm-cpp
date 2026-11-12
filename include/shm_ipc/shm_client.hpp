/**
 * @file shm_client.hpp
 * @brief 共享内存同步客户端实现（C++17）：shm::ShmClient 实现 comm::ITcpClient
 *
 * 同步阻塞式裸字节流接口，适用于命令行工具、脚本式调用等
 * 不需要事件循环的场景：
 * @code
 * shm::ShmClient client;
 * if (client.Connect("shm:///tmp/app.socket"))
 * {
 *     client.SendMessage(buf, len);        // 原始字节流写入
 *     int n = client.ReadMessage(rbuf, sizeof(rbuf));  // 阻塞读取
 *     client.Close();
 * }
 * @endcode
 *
 * shm 实现的特点：
 * - 数据面是共享内存字节流；SendMessage 在环空间不足时返回 -1 而非阻塞
 *   （共享内存没有对端消费通知写方的反向通道，无法睡眠等待空间）
 * - ReadMessage 默认无限阻塞；带 timeout_ms 的重载为扩展接口
 * - Close() 线程安全：可从其他线程调用以唤醒阻塞在 ReadMessage 的读线程
 *   （shutdown socket 使其 poll 立即返回）；channel_ 与 socket fd 的实际
 *   回收延迟到下一次 Connect() 或析构。调用 Connect()/析构前须确保
 *   ReadMessage 已返回（Close 唤醒后其会立即退出）
 * - Connect(ip, port) 形式仅作编译兼容（shm 为本地 IPC），恒返回 false
 * - 进程需自行忽略 SIGPIPE
 */

#ifndef SHM_IPC_SHM_CLIENT_HPP_
#define SHM_IPC_SHM_CLIENT_HPP_

#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string_view>

#include <comm/client.h>

#include "common.hpp"
#include "ring_channel.hpp"

namespace shm {

/**
 * @brief 共享内存同步客户端（实现 comm::ITcpClient，对应框架 io::tcp::Client<T>）
 */
class ShmClient : public comm::ITcpClient
{
 public:
    ShmClient() = default;
    ~ShmClient() override { Close(); }

    // 禁止复制和移动（持有 channel 状态）
    ShmClient(const ShmClient&)            = delete;
    ShmClient& operator=(const ShmClient&) = delete;
    ShmClient(ShmClient&&)                 = delete;
    ShmClient& operator=(ShmClient&&)      = delete;

    // ------------------------------------------------------------------
    // comm::ITcpClient 接口实现
    // ------------------------------------------------------------------

    /**
     * @brief 连接服务端
     *
     * uri 支持 "shm:///path"、"uds:///path" 或裸路径。
     * 同步完成 connect + 握手（5 秒超时）。
     */
    bool Connect(const std::string_view& uri) override
    {
        Close();  // 唤醒可能阻塞的读线程（如有）
        // 旧通道与 socket 此时回收：调用方须保证没有仍在执行的
        // ReadMessage（Close 唤醒后其立即返回）
        channel_ = DefaultRingChannel{};
        socket_fd_.Reset();
        last_error_     = ErrorCode::kOk;
        last_sys_errno_ = 0;

        UniqueFd sock;
        ErrorCode ec = ConnectUnixSocket(UriToPath(uri), &sock, &last_sys_errno_);
        if (ec != ErrorCode::kOk)
        {
            last_error_ = ec;
            return false;
        }

        DefaultRingChannel channel;
        ec = DefaultRingChannel::CreateRingChannel(sock.Get(), ChannelRole::kClient, &channel, &last_sys_errno_);
        if (ec != ErrorCode::kOk)
        {
            last_error_ = ec;
            return false;
        }

        socket_fd_ = std::move(sock);
        channel_   = std::move(channel);
        closed_.store(false, std::memory_order_release);
        return true;
    }

    /**
     * @brief 编译兼容占位：shm 为本地 IPC，不支持 TCP（ip, port）形式，恒返回 false
     */
    bool Connect(const std::string_view& /*ip*/, uint16_t /*port*/) override { return false; }

    [[nodiscard]]
    bool Connected() const noexcept override
    {
        return socket_fd_.Get() >= 0 && !closed_.load(std::memory_order_acquire);
    }

    /**
     * @brief 关闭连接（线程安全，可从其他线程调用）
     *
     * 置 closed_ 标志并 shutdown socket：阻塞在 ReadMessage 的读线程
     * 会立即被唤醒并返回 -1。channel_ 与 socket fd 不在此销毁
     * （读线程可能仍在使用），回收延迟到下一次 Connect() 或析构。
     */
    void Close() override
    {
        closed_.store(true, std::memory_order_release);
        if (socket_fd_.Get() >= 0)
        {
            ::shutdown(socket_fd_.Get(), SHUT_RDWR);
        }
    }

    /**
     * @brief 发送原始字节流
     * @return 成功返回 len；未连接或环空间不足返回 -1（不阻塞、不半截写入）
     */
    int SendMessage(const char* message, const int len) override
    {
        if (!Connected() || !message || len <= 0)
        {
            return -1;
        }
        if (channel_.WritableBytes() < static_cast<uint64_t>(len))
        {
            return -1;  // 环满（对端消费过慢），整体失败
        }

        int sent = 0;
        while (sent < len)
        {
            // 单次 TryWrite 上限为 kMaxWriteSize（Capacity/2）
            uint32_t chunk = static_cast<uint32_t>(len - sent);
            if (chunk > DefaultRingChannel::kMaxWriteSize)
            {
                chunk = static_cast<uint32_t>(DefaultRingChannel::kMaxWriteSize);
            }
            if (channel_.TryWrite(message + sent, chunk) != 0)
            {
                return -1;  // 理论不可达（已预检空间）
            }
            sent += static_cast<int>(chunk);
        }
        channel_.NotifyPeer();
        return sent;
    }

    /**
     * @brief 阻塞读取：直到读到数据或对端关闭
     * @return 实际读取的字节数（>0）；对端关闭返回 0；本端 Close() 或
     *         未连接返回 -1
     */
    int ReadMessage(char* buf, size_t bsize) override { return ReadMessage(buf, bsize, -1); }

    // ------------------------------------------------------------------
    // shm 扩展接口（非接口方法）
    // ------------------------------------------------------------------

    /**
     * @brief 带超时的阻塞读取（扩展接口）
     * @param timeout_ms 超时毫秒数，-1 表示无限等待
     * @return 实际读取的字节数（>0）；对端关闭返回 0；超时返回 -2；
     *         本端 Close() 或未连接返回 -1
     */
    int ReadMessage(char* buf, size_t bsize, int timeout_ms)
    {
        if (!Connected())
        {
            return -1;
        }

        timespec deadline{};
        timespec* dl = InitDeadline(timeout_ms, &deadline);
        for (;;)
        {
            // 本端 Close()：立即退出，不再等待
            if (closed_.load(std::memory_order_acquire))
            {
                return -1;
            }

            // 先试读：数据可能已在环中（基线须在读之前取，供 PrepareWait 复查）
            const uint64_t seen = channel_.WritePos();
            uint32_t n          = channel_.TryRead(buf, static_cast<uint32_t>(bsize));
            if (n > 0)
            {
                return static_cast<int>(n);
            }

            const int wr = WaitForReadable(dl, seen);
            if (wr < 0)
            {
                return wr;  // -1 本端关闭/错误；-2 超时
            }
            if (wr == 0)
            {
                // 对端关闭：最后再读一次环中残留数据
                n = channel_.TryRead(buf, static_cast<uint32_t>(bsize));
                return n > 0 ? static_cast<int>(n) : 0;
            }
        }
    }

    /**
     * @brief 零拷贝 Peek：等待数据到达，返回环内可读数据段（扩展接口）
     *
     * 与 ReadMessage 的等待语义一致，但不在库内拷贝：返回的指针直接指向
     * ring 数据区（镜像映射启用时永远是单段；否则可能分为两段）。
     * 调用方处理完数据后必须调用 CommitMessage(total) 推进 read_pos。
     *
     * @param seg1      输出：第一段数据指针
     * @param seg1_len  输出：第一段长度
     * @param seg2      输出：第二段数据指针（未回绕时为 nullptr）
     * @param seg2_len  输出：第二段长度（未回绕时为 0）
     * @param timeout_ms 超时毫秒数，-1 表示无限等待
     * @return >0 可读总字节数；0 对端关闭；-1 未连接/本端关闭；-2 超时
     *
     * @note 指针在 CommitMessage / 下一次 PeekMessage / Close 之前有效。
     */
    int PeekMessage(const void** seg1, uint32_t* seg1_len, const void** seg2, uint32_t* seg2_len, int timeout_ms = -1)
    {
        if (!seg1 || !seg1_len || !seg2 || !seg2_len)
        {
            return -1;
        }
        if (!Connected())
        {
            return -1;
        }

        timespec deadline{};
        timespec* dl = InitDeadline(timeout_ms, &deadline);
        for (;;)
        {
            if (closed_.load(std::memory_order_acquire))
            {
                return -1;
            }

            const uint64_t seen = channel_.WritePos();
            if (channel_.Peek(seg1, seg1_len, seg2, seg2_len) == 0)
            {
                return static_cast<int>(*seg1_len + *seg2_len);
            }

            const int wr = WaitForReadable(dl, seen);
            if (wr < 0)
            {
                return wr;
            }
            if (wr == 0)
            {
                // 对端关闭：返回环中残留数据（若有）
                if (channel_.Peek(seg1, seg1_len, seg2, seg2_len) == 0)
                {
                    return static_cast<int>(*seg1_len + *seg2_len);
                }
                return 0;
            }
        }
    }

    /**
     * @brief 提交 PeekMessage 读取的字节数（扩展接口）
     * @param len 已消费的字节数（不得超过 PeekMessage 返回的总长度）
     */
    void CommitMessage(uint32_t len) { channel_.CommitRead(len); }

    /// @brief 最近一次 Connect 的错误码（扩展接口）
    [[nodiscard]]
    ErrorCode LastError() const noexcept
    {
        return last_error_;
    }

    /// @brief 最近一次 Connect 失败时的 errno（扩展接口）
    [[nodiscard]]
    int LastSysErrno() const noexcept
    {
        return last_sys_errno_;
    }

    /// @brief 底层 socket fd（扩展接口，调试/集成用）
    [[nodiscard]]
    int GetSockFd() const
    {
        return socket_fd_.Get();
    }

    /// @brief 访问底层通道（批量写入等高级用法）
    DefaultRingChannel& Channel() noexcept { return channel_; }

 private:
    /// @brief 初始化超时 deadline；timeout_ms < 0 返回 nullptr（无限等待）
    static timespec* InitDeadline(int timeout_ms, timespec* out) noexcept
    {
        if (timeout_ms < 0)
        {
            return nullptr;
        }
        ::clock_gettime(CLOCK_MONOTONIC, out);
        out->tv_sec += timeout_ms / 1000;
        out->tv_nsec += (timeout_ms % 1000) * 1000000L;
        out->tv_sec += out->tv_nsec / 1000000000L;
        out->tv_nsec %= 1000000000L;
        return out;
    }

    /**
     * @brief 等待 ring 数据/对端事件
     * @param deadline 为空表示无限等待
     * @param seen     调用方读环检查前观察到的 write_pos（用于按需通知复查）
     * @return 1 需要重新检查 ring；0 对端关闭；-1 本端关闭/错误；-2 超时
     */
    int WaitForReadable(timespec* deadline, uint64_t seen)
    {
        int wait_ms = -1;
        if (deadline)
        {
            timespec now{};
            ::clock_gettime(CLOCK_MONOTONIC, &now);
            const int64_t ms = (deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
            if (ms <= 0)
            {
                return -2;
            }
            wait_ms = static_cast<int>(ms);
        }

        // 声明即将睡眠：写方此后才会写 eventfd；复查发现新数据则直接重试读取
        if (!channel_.PrepareWait(seen))
        {
            return 1;
        }

        pollfd pfds[2] = {{channel_.NotifyReadFd(), POLLIN, 0}, {socket_fd_.Get(), POLLIN, 0}};
        const int pr   = ::poll(pfds, 2, wait_ms);
        if (pr < 0)
        {
            return (errno == EINTR) ? 1 : -1;
        }
        if (pr == 0)
        {
            return -2;
        }

        // socket 可读 = 对端关闭或本端 Close() 的 shutdown 唤醒
        if (pfds[1].revents & (POLLHUP | POLLERR | POLLIN))
        {
            if (closed_.load(std::memory_order_acquire))
            {
                return -1;
            }
            char b{};
            if (::read(socket_fd_.Get(), &b, 1) <= 0)
            {
                socket_fd_.Reset();
                return 0;  // 对端关闭
            }
        }
        if (pfds[0].revents & POLLIN)
        {
            DefaultRingChannel::DrainNotify(pfds[0].fd);
        }
        return 1;
    }

    UniqueFd socket_fd_;                     ///< 连接 socket（断连检测）
    DefaultRingChannel channel_;             ///< 双向共享内存通道
    std::atomic<bool> closed_{true};         ///< 本端已关闭标志（Close/读线程跨线程通信）
    ErrorCode last_error_ = ErrorCode::kOk;  ///< 最近一次 Connect 错误码
    int last_sys_errno_   = 0;               ///< 最近一次 Connect 的 errno
};

}  // namespace shm

#endif  // SHM_IPC_SHM_CLIENT_HPP_
