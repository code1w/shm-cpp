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
#include <sys/stat.h>
#include <sys/time.h>

#include <cerrno>

#include "common.hpp"
#include "error.hpp"
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
 *     ChannelBatchWriter batch;
 *     if (channel.StartBatch(&batch) == ErrorCode::kOk)
 *     {
 *         batch.TryWrite(data1, len1);
 *         batch.TryWrite(data2, len2);
 *         batch.Flush();  // store write_pos + NotifyPeer()
 *     }
 * }
 * @endcode
 *
 * @tparam Capacity 通道环形缓冲区容量
 */
template <std::size_t Capacity>
class ChannelBatchWriter
{
 public:
    /** @brief 构造惰性写入器（未绑定通道；Flush 不通知） */
    ChannelBatchWriter() = default;

    ChannelBatchWriter(typename RingBuf<Capacity>::BatchWriter&& bw, const RingChannel<Capacity>* ch)
        : batch_(std::move(bw)), channel_(ch)
    {}

    ~ChannelBatchWriter() { Flush(); }

    ChannelBatchWriter(ChannelBatchWriter&& o) noexcept : batch_(std::move(o.batch_)), channel_(o.channel_)
    {
        o.channel_ = nullptr;
    }

    /** @brief 移动赋值：丢弃目标未发布数据后接管源对象 */
    ChannelBatchWriter& operator=(ChannelBatchWriter&& o) noexcept
    {
        if (this != &o)
        {
            batch_.Cancel();
            batch_     = std::move(o.batch_);
            channel_   = o.channel_;
            o.channel_ = nullptr;
        }
        return *this;
    }

    ChannelBatchWriter(const ChannelBatchWriter&)            = delete;
    ChannelBatchWriter& operator=(const ChannelBatchWriter&) = delete;

    /**
     * @brief 尝试写入一段数据（不触发原子 store 和通知）
     * @param data 待写入数据
     * @param len  数据长度（字节）
     * @return 0 成功，-1 空间不足或数据过大
     */
    int TryWrite(const void* data, uint32_t len) { return batch_.TryWrite(data, len); }

    /**
     * @brief 发布所有累积写入并通知对端
     * @return 自上次 Flush 以来的 TryWrite 成功调用次数
     */
    int Flush()
    {
        const int n = batch_.Flush();
        if (n > 0 && channel_)
        {
            channel_->NotifyPeer();
        }
        return n;
    }

    /** @brief 自上次 Flush 以来已成功 TryWrite 的次数 */
    [[nodiscard]]
    int Count() const noexcept
    {
        return batch_.Count();
    }

    /** @brief 自上次 Flush 以来已写入但尚未发布的字节数 */
    [[nodiscard]]
    uint64_t PendingBytes() const noexcept
    {
        return batch_.PendingBytes();
    }

    /** @brief 丢弃自上次 Flush 以来的所有未发布写入（析构时不再发布） */
    void Cancel() noexcept { batch_.Cancel(); }

    /** @brief 回滚到之前的 PendingBytes/Count 状态（撤销最近一次帧写入） */
    void RewindTo(uint64_t pending, int count) noexcept { batch_.RewindTo(pending, count); }

    /** @brief 当前可写入的剩余字节数 */
    [[nodiscard]]
    uint64_t FreeBytes() const noexcept
    {
        return batch_.FreeBytes();
    }

    /** @brief 预留连续写入区域，不跨环尾时返回直写指针 */
    char* Reserve(uint32_t len) { return batch_.Reserve(len); }

    /** @brief 提交 Reserve 预留的写入 */
    void CommitReserve(uint32_t len) { batch_.CommitReserve(len); }

 private:
    typename RingBuf<Capacity>::BatchWriter batch_;
    const RingChannel<Capacity>* channel_ = nullptr;
};

}  // namespace detail

/**
 * @brief 双向共享内存环形通道
 *
 * @thread-safety 每个方向为 SPSC：写环只能由一个线程写入，读环只能由一个线程读取
 *                （读写两端可以是不同线程/进程）。同一方向的并发写或并发读
 *                是未定义行为；通道对象本身（含 move）不是线程安全的。
 *
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
    RingChannel(RingChannel&&)                 = default;
    RingChannel& operator=(RingChannel&&)      = default;
    RingChannel(const RingChannel&)            = delete;
    RingChannel& operator=(const RingChannel&) = delete;

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
     * @param out       成功时写入初始化完成的 RingChannel（失败时保持原状）
     * @param sys_errno 可选：失败时的 errno
     * @return kOk 成功；kSizeMismatch 对端环尺寸不符；其余见 ErrorCode
     */
    [[nodiscard]]
    static ErrorCode CreateRingChannel(int socket_fd, ChannelRole role, RingChannel* out,
                                       int* sys_errno = nullptr) noexcept
    {
        if (!out || socket_fd < 0)
        {
            return ErrorCode::kInvalidArgument;
        }

        RingChannel ch;
        const detail::HandshakeGuard guard{socket_fd};  // 握手超时，返回时恢复

        ErrorCode ec = ErrorCode::kOk;
        if (role == ChannelRole::kClient)
        {
            ec = ch.CreateWriteRing("client_to_server", sys_errno);
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
            ec = CreateEventfd(&ch.notify_read_efd_, sys_errno);
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
            ec = ch.SendLocalFds(socket_fd, sys_errno);  // 先发：memfd_c2s、efd_client
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
            ec = ch.RecvPeerFds(socket_fd, sys_errno);  // 后收：memfd_s2c、efd_server
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
        }
        else
        {
            ec = ch.RecvPeerFds(socket_fd, sys_errno);  // 先收：memfd_c2s、efd_client
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
            ec = ch.CreateWriteRing("server_to_client", sys_errno);
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
            ec = CreateEventfd(&ch.notify_read_efd_, sys_errno);
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
            ec = ch.SendLocalFds(socket_fd, sys_errno);  // 后发：memfd_s2c、efd_server
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
        }

        *out = std::move(ch);
        return ErrorCode::kOk;
    }

    // ---- 数据操作 ----

    /**
     * @brief 尝试写入字节数据
     * @param data 待写入数据
     * @param len  数据长度（字节）
     * @return 0 成功，-1 缓冲区满或数据过大
     */
    int TryWrite(const void* data, uint32_t len)
    {
        if (!write_region_)
        {  // 默认构造 / moved-from：无写环
            return -1;
        }
        if (write_region_.Mirrored())
        {
            return Ring::TryWriteContig(write_region_.Get(), data, len);
        }
        return Ring::TryWrite(write_region_.Get(), data, len);
    }

    /**
     * @brief 读取最多 max_len 字节
     * @param data    输出缓冲区
     * @param max_len 最大读取字节数
     * @return 实际读取的字节数（0 表示缓冲区为空）
     */
    uint32_t TryRead(void* data, uint32_t max_len)
    {
        if (!read_region_)
        {
            return 0;
        }
        if (read_region_.Mirrored())
        {
            return Ring::TryReadContig(read_region_.Get(), data, max_len);
        }
        return Ring::TryRead(read_region_.Get(), data, max_len);
    }

    /**
     * @brief 精确读取 len 字节，数据不足时不消费任何字节
     * @param data 输出缓冲区（至少 len 字节）
     * @param len  需要读取的精确字节数
     * @return 0 成功，-1 数据不足
     */
    int ReadExact(void* data, uint32_t len)
    {
        if (!read_region_)
        {
            return -1;
        }
        if (read_region_.Mirrored())
        {
            return Ring::ReadExactContig(read_region_.Get(), data, len);
        }
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
    int Peek(const void** seg1, uint32_t* seg1_len, const void** seg2, uint32_t* seg2_len)
    {
        if (!read_region_)
        {
            return -1;
        }
        if (read_region_.Mirrored())
        {
            if (!seg1 || !seg1_len || !seg2 || !seg2_len)
            {
                return -1;
            }
            const int rc = Ring::PeekContig(read_region_.Get(), seg1, seg1_len);
            *seg2        = nullptr;
            *seg2_len    = 0;
            return rc;
        }
        return Ring::Peek(read_region_.Get(), seg1, seg1_len, seg2, seg2_len);
    }

    /**
     * @brief 提交读取，推进 read_pos
     * @param len 要推进的字节数
     */
    void CommitRead(uint32_t len)
    {
        if (!read_region_)
        {
            return;
        }
        Ring::CommitRead(read_region_.Get(), len);
    }

    // ---- 批量写入 ----

    /**
     * @brief 创建通道级批量写入器
     * @param out 成功时写入写入器（失败时保持原状）
     * @return kOk 成功；kInvalidArgument 出参为空；kNotInitialized 通道未初始化
     */
    [[nodiscard]]
    ErrorCode StartBatch(ChannelBatchWriter* out) noexcept
    {
        if (!out)
        {
            return ErrorCode::kInvalidArgument;
        }
        if (!write_region_)
        {
            return ErrorCode::kNotInitialized;
        }
        *out = ChannelBatchWriter(typename Ring::BatchWriter(write_region_.Get(), write_region_.Mirrored()), this);
        return ErrorCode::kOk;
    }

    // ---- 状态查询 ----

    /** @brief 返回读环中可读字节数（通道未初始化时为 0） */
    [[nodiscard]]
    uint64_t Readable() const
    {
        return read_region_ ? Ring::Available(read_region_.Get()) : 0;
    }

    /** @brief 返回写环中剩余可写字节数（通道未初始化时为 0） */
    [[nodiscard]]
    uint64_t WritableBytes() const
    {
        return write_region_ ? Ring::FreeSpace(write_region_.Get()) : 0;
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
     *
     * 必须在数据发布（write_pos 已 store）之后调用。对端若启用了按需通知
     * （调用过 PrepareWait），仅在其即将睡眠时才写 eventfd；处理中的读方
     * 无需唤醒，省掉一次 syscall。对端未启用时每次都写（与旧行为一致）。
     */
    void NotifyPeer() const
    {
        if (!notify_write_efd_)
        {  // 未完成握手 / moved-from
            return;
        }
        if (write_region_ && !Ring::ShouldNotify(write_region_.Get()))
        {
            return;
        }
        uint64_t v = 1;
        ssize_t r  = 0;
        do
        {
            r = ::write(notify_write_efd_.Get(), &v, sizeof(v));
        } while (r < 0 && errno == EINTR);
    }

    /**
     * @brief 读方：睡眠（回到 poll / 阻塞等待）前调用，声明需要被唤醒
     *
     * 调用方必须在"已读空/已处理完"之后、真正睡眠之前调用。返回 false
     * 表示 write_pos 相对基线已变化（有新数据到达），此时不得睡眠，应继续读取。
     *
     * @param seen_write 读方上一轮处理开始前观察到的 write_pos（见 WritePos()）；
     *                   以"环空"为睡眠条件的读方可传 ReadPos()
     * @return true 可以睡眠；false 有新数据，需继续读取
     */
    [[nodiscard]]
    bool PrepareWait(uint64_t seen_write)
    {
        if (!read_region_)
        {
            return true;
        }
        return Ring::ArmReaderWait(read_region_.Get(), seen_write);
    }

    /** @brief 读环当前 write_pos（通道未初始化时为 0） */
    [[nodiscard]]
    uint64_t WritePos() const
    {
        return read_region_ ? Ring::WritePos(read_region_.Get()) : 0;
    }

    /** @brief 读环当前 read_pos（通道未初始化时为 0） */
    [[nodiscard]]
    uint64_t ReadPos() const
    {
        return read_region_ ? Ring::ReadPos(read_region_.Get()) : 0;
    }

    /**
     * @brief 排空 eventfd（在通知回调中调用）
     * @param efd eventfd 文件描述符
     * @return 读出的计数值
     */
    static uint64_t DrainNotify(int efd)
    {
        uint64_t v = 0;
        ssize_t r  = 0;
        do
        {
            r = ::read(efd, &v, sizeof(v));
        } while (r < 0 && errno == EINTR);
        return v;
    }

    /// @brief 单次写入最大字节数
    static constexpr std::size_t kMaxWriteSize = Ring::kMaxWriteSize;

    /// @brief 写环数据区是否启用镜像映射（可单次跨尾拷贝）
    [[nodiscard]]
    bool WriteMirrored() const noexcept
    {
        return write_region_.Mirrored();
    }

    /// @brief 读环数据区是否启用镜像映射（Peek 永远单段）
    [[nodiscard]]
    bool ReadMirrored() const noexcept
    {
        return read_region_.Mirrored();
    }

 private:
    /// @brief 创建写环：memfd + mmap + 清零初始化
    /// @param name      memfd 名称（调试用）
    /// @param sys_errno 可选：失败时的 errno
    ErrorCode CreateWriteRing(const char* name, int* sys_errno = nullptr) noexcept
    {
        ErrorCode ec = CreateMemfd(name, Ring::kShmSize, &write_memfd_, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            return ec;
        }
        ec = write_region_.MapMirrored(write_memfd_.Get(), Ring::kShmSize, kRingDataOffset, Ring::kCapacity,
                                       PROT_READ | PROT_WRITE, MAP_SHARED, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            // 镜像仅影响本地 VA 布局；失败时退化为普通单次映射
            ec = write_region_.Map(write_memfd_.Get(), Ring::kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, sys_errno);
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
        }
        Ring::Init(write_region_.Get());
        return ErrorCode::kOk;
    }

    /// @brief 发送本端 fd：写环 memfd + "唤醒自身"的 eventfd
    ErrorCode SendLocalFds(int socket_fd, int* sys_errno = nullptr) const noexcept
    {
        const ErrorCode ec = SendFd(socket_fd, write_memfd_.Get(), FdTag::kMemfd, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            return ec;
        }
        return SendFd(socket_fd, notify_read_efd_.Get(), FdTag::kEventfd, sys_errno);
    }

    /// @brief 接收对端 fd：读环 memfd（mmap 挂载）+ "唤醒对端"的 eventfd
    ErrorCode RecvPeerFds(int socket_fd, int* sys_errno = nullptr) noexcept
    {
        ErrorCode ec = RecvFdExpect(socket_fd, FdTag::kMemfd, &read_memfd_, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            return ec;
        }

        // 必须先校验对端 memfd 尺寸再 mmap/访问：
        // 两端 Capacity 不一致或对端发送 0 字节/被截断的 memfd 时，
        // 按本地 Ring::kShmSize 映射并在环尾读写会越过文件尾（SIGBUS）
        // 或越过本地映射（SIGSEGV）。
        struct stat st
        {};
        if (::fstat(read_memfd_.Get(), &st) != 0)
        {
            if (sys_errno)
            {
                *sys_errno = errno;
            }
            return ErrnoToErrorCode(errno);
        }
        if (!S_ISREG(st.st_mode) || st.st_size != static_cast<off_t>(Ring::kShmSize))
        {
            return ErrorCode::kSizeMismatch;
        }

        ec = read_region_.MapMirrored(read_memfd_.Get(), Ring::kShmSize, kRingDataOffset, Ring::kCapacity,
                                      PROT_READ | PROT_WRITE, MAP_SHARED, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            ec = read_region_.Map(read_memfd_.Get(), Ring::kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, sys_errno);
            if (ec != ErrorCode::kOk)
            {
                return ec;
            }
        }
        return RecvFdExpect(socket_fd, FdTag::kEventfd, &notify_write_efd_, sys_errno);
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
