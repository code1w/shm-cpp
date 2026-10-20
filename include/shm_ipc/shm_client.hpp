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
    ShmClient(const ShmClient &)            = delete;
    ShmClient &operator=(const ShmClient &) = delete;
    ShmClient(ShmClient &&)                 = delete;
    ShmClient &operator=(ShmClient &&)      = delete;

    // ------------------------------------------------------------------
    // comm::ITcpClient 接口实现
    // ------------------------------------------------------------------

    /**
     * @brief 连接服务端
     *
     * uri 支持 "shm:///path"、"uds:///path" 或裸路径。
     * 同步完成 connect + 握手（5 秒超时）。
     */
    bool Connect(const std::string_view &uri) override
    {
        Close();  // 唤醒可能阻塞的读线程（如有）
        // 旧通道与 socket 此时回收：调用方须保证没有仍在执行的
        // ReadMessage（Close 唤醒后其立即返回）
        channel_   = DefaultRingChannel{};
        socket_fd_.Reset();
        try
        {
            socket_fd_ = ConnectUnixSocket(UriToPath(uri).c_str());
            channel_   = DefaultRingChannel::CreateRingChannel(socket_fd_.Get(),
                                                               ChannelRole::kClient);
        }
        catch (const std::exception &)
        {
            socket_fd_.Reset();
            return false;
        }
        closed_.store(false, std::memory_order_release);
        return true;
    }

    /**
     * @brief 编译兼容占位：shm 为本地 IPC，不支持 TCP（ip, port）形式，恒返回 false
     */
    bool Connect(const std::string_view & /*ip*/, uint16_t /*port*/) override
    {
        return false;
    }

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
            ::shutdown(socket_fd_.Get(), SHUT_RDWR);
    }

    /**
     * @brief 发送原始字节流
     * @return 成功返回 len；未连接或环空间不足返回 -1（不阻塞、不半截写入）
     */
    int SendMessage(const char *message, const int len) override
    {
        if (!Connected() || !message || len <= 0)
            return -1;
        if (channel_.WritableBytes() < static_cast<uint64_t>(len))
            return -1;  // 环满（对端消费过慢），整体失败

        int sent = 0;
        while (sent < len)
        {
            // 单次 TryWrite 上限为 max_write_size（Capacity/2）
            uint32_t chunk = static_cast<uint32_t>(len - sent);
            if (chunk > DefaultRingChannel::max_write_size)
                chunk = static_cast<uint32_t>(DefaultRingChannel::max_write_size);
            if (channel_.TryWrite(message + sent, chunk) != 0)
                return -1;  // 理论不可达（已预检空间）
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
    int ReadMessage(char *buf, size_t bsize) override
    {
        return ReadMessage(buf, bsize, -1);
    }

    // ------------------------------------------------------------------
    // shm 扩展接口（非接口方法）
    // ------------------------------------------------------------------

    /**
     * @brief 带超时的阻塞读取（扩展接口）
     * @param timeout_ms 超时毫秒数，-1 表示无限等待
     * @return 实际读取的字节数（>0）；对端关闭返回 0；超时返回 -2；
     *         本端 Close() 或未连接返回 -1
     */
    int ReadMessage(char *buf, size_t bsize, int timeout_ms)
    {
        if (!Connected())
            return -1;

        timespec deadline{};
        if (timeout_ms >= 0)
        {
            ::clock_gettime(CLOCK_MONOTONIC, &deadline);
            deadline.tv_sec  += timeout_ms / 1000;
            deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
            deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
        }

        int efd = channel_.NotifyReadFd();
        for (;;)
        {
            // 本端 Close()：立即退出，不再等待（close 唤醒的 poll 走这里
            // 或下方 socket 分支，两处都检查）
            if (closed_.load(std::memory_order_acquire))
                return -1;

            // 先试读：数据可能已在环中
            uint32_t n = channel_.TryRead(buf, static_cast<uint32_t>(bsize));
            if (n > 0)
                return static_cast<int>(n);

            int wait_ms = timeout_ms;
            if (timeout_ms >= 0)
            {
                timespec now{};
                ::clock_gettime(CLOCK_MONOTONIC, &now);
                int64_t ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                             (deadline.tv_nsec - now.tv_nsec) / 1000000;
                if (ms <= 0)
                    return -2;  // 超时
                wait_ms = static_cast<int>(ms);
            }

            pollfd pfds[2] = {{efd, POLLIN, 0}, {socket_fd_.Get(), POLLIN, 0}};
            int pr = ::poll(pfds, 2, wait_ms);
            if (pr < 0)
            {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (pr == 0)
                return -2;  // 超时

            // socket 可读 = 对端关闭或本端 Close() 的 shutdown 唤醒
            if (pfds[1].revents & (POLLHUP | POLLERR | POLLIN))
            {
                if (closed_.load(std::memory_order_acquire))
                    return -1;  // 本端 Close()
                char b{};
                if (::read(socket_fd_.Get(), &b, 1) <= 0)
                {
                    // 最后再读一次环中残留数据
                    n = channel_.TryRead(buf, static_cast<uint32_t>(bsize));
                    if (n > 0)
                        return static_cast<int>(n);
                    socket_fd_.Reset();
                    return 0;  // 对端关闭（同框架返回 0）
                }
            }
            if (pfds[0].revents & POLLIN)
                DefaultRingChannel::DrainNotify(efd);
        }
    }

    /// @brief 底层 socket fd（扩展接口，调试/集成用）
    int GetSockFd() const { return socket_fd_.Get(); }

    /// @brief 访问底层通道（批量写入等高级用法）
    DefaultRingChannel &Channel() noexcept { return channel_; }

 private:
    UniqueFd socket_fd_;          ///< 连接 socket（断连检测）
    DefaultRingChannel channel_;  ///< 双向共享内存通道
    std::atomic<bool> closed_{true};  ///< 本端已关闭标志（Close/读线程跨线程通信）
};

}  // namespace shm

#endif  // SHM_IPC_SHM_CLIENT_HPP_
