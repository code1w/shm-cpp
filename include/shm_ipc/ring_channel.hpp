/**
 * @file ring_channel.hpp
 * @brief 双向共享内存环形通道（C++17）
 *
 * 封装一对字节环形缓冲区（一写一读），并包含 memfd 创建、
 * eventfd 创建及 fd 交换握手逻辑。
 *
 * 通知机制：每个方向拥有一个共享 eventfd。读方创建后通过
 * SCM_RIGHTS 发送给写方。写方调用 NotifyPeer() 写入 eventfd；
 * 读方轮询 NotifyReadFd() 的 POLLIN 事件。
 */

#ifndef SHM_IPC_RING_CHANNEL_HPP_
#define SHM_IPC_RING_CHANNEL_HPP_

#include "common.hpp"
#include "ringbuf.hpp"

#include <sys/mman.h>

namespace shm_ipc {

/**
 * @brief 双向共享内存环形通道
 * @tparam Capacity 每方向环形缓冲区容量（字节），默认 8MB
 */
template <std::size_t Capacity = 8 * 1024 * 1024>
class RingChannel
{
 public:
    using Ring = RingBuf<Capacity>;

    RingChannel() = default;

    // 移动语义（持有 memfd、mmap 区域、eventfd），禁止复制
    RingChannel(RingChannel &&)                 = default;
    RingChannel &operator=(RingChannel &&)      = default;
    RingChannel(const RingChannel &)            = delete;
    RingChannel &operator=(const RingChannel &) = delete;

    /**
     * @brief 客户端工厂方法
     *
     * 握手顺序（4 次 fd 交换）：
     *   1. 发送 memfd_c2s        →  服务端
     *   2. 发送 efd_client       →  服务端（服务端写入以唤醒客户端）
     *   3. 接收 memfd_s2c        ←  服务端
     *   4. 接收 efd_server       ←  服务端（客户端写入以唤醒服务端）
     *
     * @param socket_fd 已连接的 Unix socket fd
     * @return 初始化完成的 RingChannel
     */
    [[nodiscard]]
    static RingChannel Connect(int socket_fd)
    {
        RingChannel ch;

        // 创建写环（client→server）
        ch.write_memfd_  = CreateMemfd("client_to_server", Ring::shm_size);
        ch.write_region_ = MmapRegion{ch.write_memfd_.Get(), Ring::shm_size,
                                      PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::Init(ch.write_region_.Get());

        // 创建"唤醒自身"的 eventfd
        ch.notify_read_efd_ = CreateEventfd();

        // 发送：memfd_c2s、efd_client
        SendFd(socket_fd, ch.write_memfd_.Get(), FdTag::kMemfd);
        SendFd(socket_fd, ch.notify_read_efd_.Get(), FdTag::kEventfd);

        // 接收：memfd_s2c、efd_server
        ch.read_memfd_       = RecvFdExpect(socket_fd, FdTag::kMemfd);
        ch.read_region_      = MmapRegion{ch.read_memfd_.Get(), Ring::shm_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED};
        ch.notify_write_efd_ = RecvFdExpect(socket_fd, FdTag::kEventfd);

        return ch;
    }

    /**
     * @brief 服务端工厂方法
     *
     * 握手顺序（4 次 fd 交换）：
     *   1. 接收 memfd_c2s        ←  客户端
     *   2. 接收 efd_client       ←  客户端（服务端写入以唤醒客户端）
     *   3. 发送 memfd_s2c        →  客户端
     *   4. 发送 efd_server       →  客户端（客户端写入以唤醒服务端）
     *
     * @param socket_fd 已接受的 Unix socket fd
     * @return 初始化完成的 RingChannel
     */
    [[nodiscard]]
    static RingChannel Accept(int socket_fd)
    {
        RingChannel ch;

        // 接收：memfd_c2s、efd_client
        ch.read_memfd_       = RecvFdExpect(socket_fd, FdTag::kMemfd);
        ch.read_region_      = MmapRegion{ch.read_memfd_.Get(), Ring::shm_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED};
        ch.notify_write_efd_ = RecvFdExpect(socket_fd, FdTag::kEventfd);

        // 创建写环（server→client）
        ch.write_memfd_  = CreateMemfd("server_to_client", Ring::shm_size);
        ch.write_region_ = MmapRegion{ch.write_memfd_.Get(), Ring::shm_size,
                                      PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::Init(ch.write_region_.Get());

        // 创建"唤醒自身"的 eventfd
        ch.notify_read_efd_ = CreateEventfd();

        // 发送：memfd_s2c、efd_server
        SendFd(socket_fd, ch.write_memfd_.Get(), FdTag::kMemfd);
        SendFd(socket_fd, ch.notify_read_efd_.Get(), FdTag::kEventfd);

        return ch;
    }

    // ---- 数据操作 ----

    /**
     * @brief 尝试写入消息
     * @param data payload 数据
     * @param len  payload 长度
     * @param seq  消息序列号
     * @return 0 成功，-1 缓冲区满或消息过大
     */
    int TryWrite(const void *data, uint32_t len, uint32_t seq)
    {
        return Ring::TryWrite(write_region_.Get(), data, len, seq);
    }

    /**
     * @brief 强制写入：缓冲区满时丢弃最旧消息
     * @param data payload 数据
     * @param len  payload 长度
     * @param seq  消息序列号
     */
    void ForceWrite(const void *data, uint32_t len, uint32_t seq)
    {
        Ring::ForceWrite(write_region_.Get(), data, len, seq);
    }

    /**
     * @brief 尝试读取一条消息
     * @param data 输出缓冲区
     * @param len  输出：payload 长度
     * @param seq  输出：消息序列号
     * @return 0 成功，-1 缓冲区为空
     */
    int TryRead(void *data, uint32_t *len, uint32_t *seq)
    {
        return Ring::TryRead(read_region_.Get(), data, len, seq);
    }

    /**
     * @brief 零拷贝读取（不推进 read_pos，需配合 CommitRead 使用）
     * @param data 输出：指向环内 payload 的指针
     * @param len  输出：payload 长度
     * @param seq  输出：消息序列号
     * @return 0 成功，-1 缓冲区为空
     */
    int PeekRead(const void **data, uint32_t *len, uint32_t *seq)
    {
        return Ring::PeekRead(read_region_.Get(), data, len, seq);
    }

    /**
     * @brief 提交 PeekRead，推进 read_pos
     * @param len PeekRead 返回的 payload 长度
     */
    void CommitRead(uint32_t len) { Ring::CommitRead(read_region_.Get(), len); }

    /** @brief 返回读环中已使用字节数 */
    uint64_t Readable() const { return Ring::Available(read_region_.Get()); }

    /** @brief 返回写环中剩余可写字节数 */
    uint64_t WritableBytes() const
    {
        return Capacity - Ring::Available(write_region_.Get());
    }

    // ---- 通知 ----

    /**
     * @brief 返回用于轮询（POLLIN）的 eventfd，对端写入时触发
     * @return eventfd 文件描述符
     */
    [[nodiscard]]
    int NotifyReadFd() const noexcept
    {
        return notify_read_efd_.Get();
    }

    /**
     * @brief 通知对端：我们已向写环写入新数据
     */
    void NotifyPeer() const
    {
        uint64_t v = 1;
        ::write(notify_write_efd_.Get(), &v, sizeof(v));
    }

    /**
     * @brief 排空 eventfd（在通知回调中调用）
     * @param efd eventfd 文件描述符
     * @return 读出的计数值
     */
    static uint64_t DrainNotify(int efd)
    {
        uint64_t v = 0;
        ::read(efd, &v, sizeof(v));
        return v;
    }

    /// @brief 单条消息 payload 最大长度
    static constexpr std::size_t max_msg_size = Ring::max_msg_size;

 private:
    UniqueFd write_memfd_;     ///< 写环对应的 memfd
    MmapRegion write_region_;  ///< 写环的 mmap 映射区域
    UniqueFd read_memfd_;      ///< 读环对应的 memfd
    MmapRegion read_region_;   ///< 读环的 mmap 映射区域

    UniqueFd notify_write_efd_;  ///< 写入此 eventfd 以唤醒对端
    UniqueFd notify_read_efd_;   ///< 对端写入此 eventfd 以唤醒自身
};

/// @brief 默认配置别名（8MB 环，~4MB 最大消息）
using DefaultRingChannel = RingChannel<>;

}  // namespace shm_ipc

#endif  // SHM_IPC_RING_CHANNEL_HPP_
