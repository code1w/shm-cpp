/**
 * @file ring_channel.hpp
 * @brief 双向共享内存环形通道（C++17）
 *
 * 封装一对字节流环形缓冲区（一写一读），并包含 memfd 创建、
 * eventfd 创建及 fd 交换握手逻辑。
 *
 * 通知机制：每个方向拥有一个共享 eventfd。读方创建后通过
 * SCM_RIGHTS 发送给写方。写方调用 NotifyPeer() 写入 eventfd；
 * 读方轮询 NotifyReadFd() 的 POLLIN 事件。
 */

#ifndef SHM_IPC_RING_CHANNEL_HPP_
#define SHM_IPC_RING_CHANNEL_HPP_

#include <sys/mman.h>
#include <sys/time.h>

#include "common.hpp"
#include "ringbuf.hpp"

namespace shm {

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
        SetHandshakeTimeout(socket_fd);
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
        SetHandshakeTimeout(socket_fd);
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
     * @brief 尝试写入字节数据
     * @param data 待写入数据
     * @param len  数据长度（字节）
     * @return 0 成功，-1 缓冲区满或数据过大
     */
    int TryWrite(const void *data, uint32_t len)
    {
        return Ring::TryWrite(write_region_.Get(), data, len);
    }

    /**
     * @brief 读取最多 max_len 字节
     * @param data    输出缓冲区
     * @param max_len 最大读取字节数
     * @return 实际读取的字节数（0 表示缓冲区为空）
     */
    uint32_t TryRead(void *data, uint32_t max_len)
    {
        return Ring::TryRead(read_region_.Get(), data, max_len);
    }

    /**
     * @brief 精确读取 len 字节，数据不足时不消费任何字节
     * @param data 输出缓冲区（至少 len 字节）
     * @param len  需要读取的精确字节数
     * @return 0 成功，-1 数据不足
     */
    int ReadExact(void *data, uint32_t len)
    {
        return Ring::ReadExact(read_region_.Get(), data, len);
    }

    /**
     * @brief 两段式零拷贝 Peek
     * @param seg1     输出：第一段数据指针
     * @param seg1_len 输出：第一段长度
     * @param seg2     输出：第二段数据指针（回绕部分）
     * @param seg2_len 输出：第二段长度
     * @return 0 有数据，-1 缓冲区为空
     */
    int Peek(const void **seg1, uint32_t *seg1_len,
             const void **seg2, uint32_t *seg2_len)
    {
        return Ring::Peek(read_region_.Get(), seg1, seg1_len, seg2, seg2_len);
    }

    /**
     * @brief 提交读取，推进 read_pos
     * @param len 要推进的字节数
     */
    void CommitRead(uint32_t len) { Ring::CommitRead(read_region_.Get(), len); }

    // ---- 批量写入 ----

    /**
     * @brief RAII 通道级批量写入器
     *
     * 封装 RingBuf::BatchWriter，Flush 时自动调用 NotifyPeer()。
     * 将 N 次写入的原子操作从 3N 降至 2，eventfd 通知从 N 降至 1。
     *
     * @code
     * {
     *     auto batch = channel.StartBatch();
     *     batch.TryWrite(data1, len1);
     *     batch.TryWrite(data2, len2);
     *     batch.Flush();  // store write_pos + NotifyPeer()
     * }
     * @endcode
     */
    class ChannelBatchWriter
    {
     public:
        ChannelBatchWriter(typename Ring::BatchWriter &&bw,
                           const RingChannel *ch)
            : batch_(std::move(bw)), channel_(ch) {}

        ~ChannelBatchWriter() { Flush(); }

        ChannelBatchWriter(ChannelBatchWriter &&o) noexcept
            : batch_(std::move(o.batch_)), channel_(o.channel_)
        {
            o.channel_ = nullptr;
        }

        ChannelBatchWriter &operator=(ChannelBatchWriter &&)      = delete;
        ChannelBatchWriter(const ChannelBatchWriter &)            = delete;
        ChannelBatchWriter &operator=(const ChannelBatchWriter &) = delete;

        /**
         * @brief 尝试写入一段数据（不触发原子 store 和通知）
         * @param data 待写入数据
         * @param len  数据长度（字节）
         * @return 0 成功，-1 空间不足或数据过大
         */
        int TryWrite(const void *data, uint32_t len)
        {
            return batch_.TryWrite(data, len);
        }

        /**
         * @brief 发布所有累积写入并通知对端
         * @return 自上次 Flush 以来的 TryWrite 成功调用次数
         */
        int Flush()
        {
            int n = batch_.Flush();
            if (n > 0 && channel_)
                channel_->NotifyPeer();
            return n;
        }

        /** @brief 自上次 Flush 以来已成功 TryWrite 的次数 */
        int Count() const noexcept { return batch_.Count(); }

        /** @brief 当前可写入的剩余字节数 */
        uint64_t FreeBytes() const noexcept { return batch_.FreeBytes(); }

        /** @brief 预留连续写入区域，不跨环尾时返回直写指针 */
        char *Reserve(uint32_t len) { return batch_.Reserve(len); }

        /** @brief 提交 Reserve 预留的写入 */
        void CommitReserve(uint32_t len) { batch_.CommitReserve(len); }

     private:
        typename Ring::BatchWriter batch_;
        const RingChannel *channel_;
    };

    /**
     * @brief 创建通道级批量写入器
     * @return ChannelBatchWriter 实例
     */
    [[nodiscard]]
    ChannelBatchWriter StartBatch()
    {
        return ChannelBatchWriter(
            typename Ring::BatchWriter(write_region_.Get()), this);
    }

    // ---- 状态查询 ----

    /** @brief 返回读环中可读字节数 */
    uint64_t Readable() const { return Ring::Available(read_region_.Get()); }

    /** @brief 返回写环中剩余可写字节数 */
    uint64_t WritableBytes() const { return Ring::FreeSpace(write_region_.Get()); }

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
        ssize_t r;
        do {
            r = ::write(notify_write_efd_.Get(), &v, sizeof(v));
        } while (r < 0 && errno == EINTR);
    }

    /**
     * @brief 排空 eventfd（在通知回调中调用）
     * @param efd eventfd 文件描述符
     * @return 读出的计数值
     */
    static uint64_t DrainNotify(int efd)
    {
        uint64_t v = 0;
        ssize_t r;
        do {
            r = ::read(efd, &v, sizeof(v));
        } while (r < 0 && errno == EINTR);
        return v;
    }

    /// @brief 单次写入最大字节数
    static constexpr std::size_t max_write_size = Ring::max_write_size;

 private:
    /// @brief 设置握手阶段 socket 收发超时（5秒），防止半连接阻塞
    static void SetHandshakeTimeout(int socket_fd)
    {
        timeval tv{};
        tv.tv_sec  = 5;
        tv.tv_usec = 0;
        ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    UniqueFd write_memfd_;     ///< 写环对应的 memfd
    MmapRegion write_region_;  ///< 写环的 mmap 映射区域
    UniqueFd read_memfd_;      ///< 读环对应的 memfd
    MmapRegion read_region_;   ///< 读环的 mmap 映射区域

    UniqueFd notify_write_efd_;  ///< 写入此 eventfd 以唤醒对端
    UniqueFd notify_read_efd_;   ///< 对端写入此 eventfd 以唤醒自身
};

/// @brief 默认配置别名（8MB 环）
using DefaultRingChannel = RingChannel<>;

}  // namespace shm

#endif  // SHM_IPC_RING_CHANNEL_HPP_
