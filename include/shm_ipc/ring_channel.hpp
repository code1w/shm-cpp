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

/// @brief 通道角色（决定握手时的收发顺序）
enum class ChannelRole
{
    kClient,  ///< 客户端：先建环发送，后接收
    kServer,  ///< 服务端：先接收，后建环发送
};

template <std::size_t Capacity>
class RingChannel;

// ---------------------------------------------------------------------------
// detail — RingChannel 的内部实现组件
// ---------------------------------------------------------------------------

namespace detail {

/// @brief RAII：握手期间设置 socket 收发超时（5秒，防止半连接阻塞），
///        结束（含异常路径）恢复默认阻塞模式
struct HandshakeGuard
{
    explicit HandshakeGuard(int fd) : fd_(fd)
    {
        timeval tv{};
        tv.tv_sec  = 5;
        tv.tv_usec = 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    ~HandshakeGuard()
    {
        timeval tv{};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    int fd_;
};

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
 *
 * @tparam Capacity 通道环形缓冲区容量
 */
template <std::size_t Capacity>
class ChannelBatchWriter
{
 public:
    ChannelBatchWriter(typename RingBuf<Capacity>::BatchWriter &&bw,
                       const RingChannel<Capacity> *ch)
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

    /** @brief 自上次 Flush 以来已写入但尚未发布的字节数 */
    uint64_t PendingBytes() const noexcept { return batch_.PendingBytes(); }

    /** @brief 丢弃自上次 Flush 以来的所有未发布写入（析构时不再发布） */
    void Cancel() noexcept { batch_.Cancel(); }

    /** @brief 回滚到之前的 PendingBytes/Count 状态（撤销最近一次帧写入） */
    void RewindTo(uint64_t pending, int count) noexcept
    {
        batch_.RewindTo(pending, count);
    }

    /** @brief 当前可写入的剩余字节数 */
    uint64_t FreeBytes() const noexcept { return batch_.FreeBytes(); }

    /** @brief 预留连续写入区域，不跨环尾时返回直写指针 */
    char *Reserve(uint32_t len) { return batch_.Reserve(len); }

    /** @brief 提交 Reserve 预留的写入 */
    void CommitReserve(uint32_t len) { batch_.CommitReserve(len); }

 private:
    typename RingBuf<Capacity>::BatchWriter batch_;
    const RingChannel<Capacity> *channel_;
};

}  // namespace detail

/**
 * @brief 双向共享内存环形通道
 * @tparam Capacity 每方向环形缓冲区容量（字节），默认 8MB
 */
template <std::size_t Capacity = 8 * 1024 * 1024>
class RingChannel
{
 public:
    using Ring = RingBuf<Capacity>;

    /// @brief 通道级批量写入器（实现位于 detail 命名空间）
    using ChannelBatchWriter = detail::ChannelBatchWriter<Capacity>;

    RingChannel() = default;

    // 移动语义（持有 memfd、mmap 区域、eventfd），禁止复制
    RingChannel(RingChannel &&)                 = default;
    RingChannel &operator=(RingChannel &&)      = default;
    RingChannel(const RingChannel &)            = delete;
    RingChannel &operator=(const RingChannel &) = delete;

    /**
     * @brief 创建双向环形通道（客户端/服务端统一入口）
     *
     * 握手为 4 次 fd 交换，两端互为镜像，角色只决定收发顺序：
     *
     *   客户端（kClient）              服务端（kServer）
     *   1. 发送 memfd_c2s      →       1. 接收 memfd_c2s
     *   2. 发送 efd_client     →       2. 接收 efd_client（服务端写入唤醒客户端）
     *   3. 接收 memfd_s2c      ←       3. 创建并发送 memfd_s2c
     *   4. 接收 efd_server     ←       4. 创建并发送 efd_server（客户端写入唤醒服务端）
     *
     * @param socket_fd 已建立连接的 Unix socket fd
     * @param role      本端角色
     * @return 初始化完成的 RingChannel
     */
    [[nodiscard]]
    static RingChannel CreateRingChannel(int socket_fd, ChannelRole role)
    {
        RingChannel ch;
        detail::HandshakeGuard guard{socket_fd};  // 握手超时，返回/异常时恢复

        if (role == ChannelRole::kClient)
        {
            ch.CreateWriteRing("client_to_server");
            ch.notify_read_efd_ = CreateEventfd();
            ch.SendLocalFds(socket_fd);  // 先发：memfd_c2s、efd_client
            ch.RecvPeerFds(socket_fd);   // 后收：memfd_s2c、efd_server
        }
        else
        {
            ch.RecvPeerFds(socket_fd);   // 先收：memfd_c2s、efd_client
            ch.CreateWriteRing("server_to_client");
            ch.notify_read_efd_ = CreateEventfd();
            ch.SendLocalFds(socket_fd);  // 后发：memfd_s2c、efd_server
        }
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
    /// @brief 创建写环：memfd + mmap + 清零初始化
    /// @param name memfd 名称（调试用）
    void CreateWriteRing(const char *name)
    {
        write_memfd_  = CreateMemfd(name, Ring::shm_size);
        write_region_ = MmapRegion{write_memfd_.Get(), Ring::shm_size,
                                   PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::Init(write_region_.Get());
    }

    /// @brief 发送本端 fd：写环 memfd + "唤醒自身"的 eventfd
    void SendLocalFds(int socket_fd) const
    {
        SendFd(socket_fd, write_memfd_.Get(), FdTag::kMemfd);
        SendFd(socket_fd, notify_read_efd_.Get(), FdTag::kEventfd);
    }

    /// @brief 接收对端 fd：读环 memfd（mmap 挂载）+ "唤醒对端"的 eventfd
    void RecvPeerFds(int socket_fd)
    {
        read_memfd_  = RecvFdExpect(socket_fd, FdTag::kMemfd);
        read_region_ = MmapRegion{read_memfd_.Get(), Ring::shm_size,
                                  PROT_READ | PROT_WRITE, MAP_SHARED};
        notify_write_efd_ = RecvFdExpect(socket_fd, FdTag::kEventfd);
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
