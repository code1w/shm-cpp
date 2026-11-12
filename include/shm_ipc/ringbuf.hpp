/**
 * @file ringbuf.hpp
 * @brief 字节级无锁 SPSC 环形缓冲区（C++17）
 *
 * 纯字节流环形缓冲区，无消息帧头、无哨兵、无对齐填充。
 * 消息边界由上层（codec.hpp）管理。
 *
 * 内存布局：
 * @code
 * ┌──────────────────────────────────────────────┐
 * │ RingHeader（控制区，缓存行对齐填充）          │
 * ├──────────────────────────────────────────────┤
 * │ [byte][byte][byte] ...                       │
 * │         （连续字节环）                        │
 * └──────────────────────────────────────────────┘
 * @endcode
 *
 * 回绕处理：写入数据跨越环尾时自动拆分为两段 memcpy（尾部 + 头部），
 * 读取同理。无需哨兵标记。
 */

#ifndef SHM_IPC_RINGBUF_HPP_
#define SHM_IPC_RINGBUF_HPP_

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <new>

namespace shm {

// ---------------------------------------------------------------------------
// 共享内存结构（POD，无指针）
// ---------------------------------------------------------------------------

/// @brief reader_wait 取值：读方在处理中，写方无需通知
inline constexpr uint32_t kReaderAwake = 0;
/// @brief reader_wait 取值：读方即将/已经睡眠，写方需发一次通知（发出后清零）
inline constexpr uint32_t kReaderWaiting = 1;
/// @brief reader_wait 取值：读方未启用按需通知（初始值），写方每次都通知
inline constexpr uint32_t kReaderNotifyAlways = 2;

/** @brief 环形缓冲区控制头，写位置/读位置/读方等待标志各占独立缓存行 */
struct alignas(64) RingHeader
{
    std::atomic<uint64_t> write_pos;                ///< 写位置（单调递增字节偏移）
    char pad1[64 - sizeof(std::atomic<uint64_t>)];  ///< 填充至 64 字节（一个缓存行）
    std::atomic<uint64_t> read_pos;                 ///< 读位置（单调递增字节偏移）
    char pad2[64 - sizeof(std::atomic<uint64_t>)];  ///< 填充至 64 字节
    std::atomic<uint32_t> reader_wait;              ///< 读方等待标志（kReader*），读方置位、写方清零
    char pad3[64 - sizeof(std::atomic<uint32_t>)];  ///< 填充至 64 字节
};

static_assert(sizeof(RingHeader) == 192, "RingHeader must be 192 bytes (3 cache lines)");
static_assert(alignof(RingHeader) == 64, "RingHeader must be aligned to 64 bytes");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "lock-free atomics are required for cross-process shm");

/// @brief 数据区起始偏移（页对齐，供 RingChannel 建立数据区镜像映射）
inline constexpr std::size_t kRingDataOffset = 4096;
static_assert(kRingDataOffset >= sizeof(RingHeader), "data offset must leave room for RingHeader");

// ---------------------------------------------------------------------------
// detail — RingBuf 的内部实现组件
// ---------------------------------------------------------------------------

namespace detail {

/// @brief 共享内存起始地址 → RingHeader 指针
inline RingHeader* RingHdr(void* shm) { return static_cast<RingHeader*>(shm); }

/// @brief 共享内存起始地址 → RingHeader 指针（const 版本）
inline const RingHeader* RingHdr(const void* shm) { return static_cast<const RingHeader*>(shm); }

/// @brief 共享内存起始地址 → 数据区起始指针
inline char* RingData(void* shm) { return static_cast<char*>(shm) + kRingDataOffset; }

/// @brief 共享内存起始地址 → 数据区起始指针（const 版本）
inline const char* RingData(const void* shm) { return static_cast<const char*>(shm) + kRingDataOffset; }

/// @brief 单调递增偏移 → 物理偏移（要求 Capacity 为 2 的幂）
template <std::size_t Capacity>
constexpr std::size_t RingPhys(uint64_t pos)
{
    return static_cast<std::size_t>(pos & (Capacity - 1));
}

/**
 * @brief 从环形缓冲区拷贝数据到线性缓冲区（处理回绕）
 * @param base   环形缓冲区数据区域基地址
 * @param phys_r 物理读偏移
 * @param dst    输出缓冲区
 * @param len    要拷贝的字节数
 */
template <std::size_t Capacity>
void RingCopyOut(const char* base, std::size_t phys_r, void* dst, uint32_t len)
{
    std::size_t contig = Capacity - phys_r;
    auto* d            = static_cast<char*>(dst);
    if (len <= contig)
    {
        std::memcpy(d, base + phys_r, len);
    }
    else
    {
        std::memcpy(d, base + phys_r, contig);
        std::memcpy(d + contig, base, len - contig);
    }
}

/**
 * @brief RAII 批量写入器，减少原子操作次数
 *
 * 普通 TryWrite 每次写入需要 3 次原子操作（load write_pos、load read_pos、
 * store write_pos）。BatchWriter 在构造时快照一次 read_pos，写入过程中
 * 仅本地追踪 write_pos，Flush 时一次性 store，将 N 次写入的原子操作
 * 从 3N 降至 2（1 acquire load + 1 release store）。
 *
 * 使用示例：
 * @code
 * {
 *     auto batch = RingBuf<>::BatchWriter(shm);
 *     batch.TryWrite(data1, len1);
 *     batch.TryWrite(data2, len2);
 *     batch.Flush();  // 或依赖析构自动 flush
 * }
 * @endcode
 *
 * @note read_pos 快照是保守估计：读方可能已消费更多数据，
 *       但绝不会消费得更少，因此空间检查不会产生错误。
 *       代价是在读方已消费数据的场景下，可能误判为空间不足。
 *
 * @warning 同一时刻同一环上只允许存在一个活跃的 BatchWriter。
 *       每个 BatchWriter 在构造时从共享内存快照 write_pos，
 *       两个活跃实例会向同一物理位置写入并互相覆盖，造成静默的
 *       数据损坏。同理，持有活跃 ChannelBatchWriter 期间不得调用
 *       独立的 Send/SendFrame（它们内部会新建 BatchWriter）。
 *
 * @tparam Capacity 环形缓冲区容量（字节），必须为 2 的幂
 */
template <std::size_t Capacity>
class BatchWriter
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 1024, "Capacity must be at least 1024 bytes");

 public:
    static constexpr std::size_t max_write_size = Capacity / 2;

    /** @brief 构造惰性写入器（未绑定共享内存；所有写操作返回 -1） */
    BatchWriter() = default;

    /**
     * @brief 构造批量写入器，快照当前 write_pos 和 read_pos
     * @param shm 共享内存指针
     */
    explicit BatchWriter(void* shm, bool mirrored = false)
        : hdr_(RingHdr(shm)),
          base_(RingData(shm)),
          mirrored_(mirrored),
          w_(hdr_->write_pos.load(std::memory_order_relaxed)),
          r_(hdr_->read_pos.load(std::memory_order_acquire)),
          w_start_(w_)
    {}

    ~BatchWriter() { Flush(); }

    BatchWriter(BatchWriter&& o) noexcept
        : hdr_(o.hdr_),
          base_(o.base_),
          mirrored_(o.mirrored_),
          w_(o.w_),
          r_(o.r_),
          w_start_(o.w_start_),
          count_(o.count_),
          reserved_(o.reserved_),
          reserved_len_(o.reserved_len_)
    {
        // moved-from 必须是惰性对象：只清 hdr_ 而保留 base_/w_ 时，
        // 对其 TryWrite 会写入共享环并返回成功，但 Flush 永不发布；
        // 更糟的是它会覆写活 writer 尚未 Flush 的数据，造成静默损坏。
        o.hdr_  = nullptr;
        o.base_ = nullptr;
        o.w_ = o.r_ = o.w_start_ = 0;
        o.count_                 = 0;
        o.reserved_              = nullptr;
        o.reserved_len_          = 0;
    }

    /**
     * @brief 移动赋值：先丢弃目标对象未发布的数据，再接管源对象
     *
     * 选择 Cancel 而不是 Flush：赋值语义不应隐式向对端发布半成品批次。
     */
    BatchWriter& operator=(BatchWriter&& o) noexcept
    {
        if (this != &o)
        {
            Cancel();
            hdr_          = o.hdr_;
            base_         = o.base_;
            mirrored_     = o.mirrored_;
            w_            = o.w_;
            r_            = o.r_;
            w_start_      = o.w_start_;
            count_        = o.count_;
            reserved_     = o.reserved_;
            reserved_len_ = o.reserved_len_;

            o.hdr_  = nullptr;
            o.base_ = nullptr;
            o.w_ = o.r_ = o.w_start_ = 0;
            o.count_                 = 0;
            o.reserved_              = nullptr;
            o.reserved_len_          = 0;
        }
        return *this;
    }
    BatchWriter(const BatchWriter&)            = delete;
    BatchWriter& operator=(const BatchWriter&) = delete;

    /**
     * @brief 尝试写入一段数据到批量缓冲区（不触发原子 store）
     * @param data 待写入数据
     * @param len  数据长度（字节）
     * @return 0 成功，-1 空间不足或数据过大
     */
    int TryWrite(const void* data, uint32_t len)
    {
        if (!hdr_ || !base_)  // moved-from / 已失效：不得写入
            return -1;
        if (len == 0 || len > max_write_size)
            return -1;
        if (data == nullptr)  // memcpy(dst, nullptr, n) 是 UB，防御性拒绝
            return -1;

        if ((w_ + len) - r_ > Capacity)
            return -1;

        std::size_t phys_w = RingPhys<Capacity>(w_);
        std::size_t tail   = Capacity - phys_w;

        auto* src = static_cast<const char*>(data);
        if (mirrored_ || len <= tail)
        {
            // 镜像映射下数据区连续两圈，跨尾也可单次 memcpy
            std::memcpy(base_ + phys_w, src, len);
        }
        else
        {
            std::memcpy(base_ + phys_w, src, tail);
            std::memcpy(base_, src + tail, len - tail);
        }

        w_ += len;
        ++count_;
        // 直接写入会使此前的 Reserve 预留失效
        reserved_     = nullptr;
        reserved_len_ = 0;
        return 0;
    }

    /**
     * @brief 将累积的写入发布到共享内存（一次 release store）
     * @return 自上次 Flush 以来的 TryWrite 成功调用次数
     */
    int Flush()
    {
        if (!hdr_)
            return 0;
        int n = count_;
        if (w_ != w_start_)
        {
            hdr_->write_pos.store(w_, std::memory_order_release);
            w_start_ = w_;
        }
        count_ = 0;
        // 未提交的预留随本批结束而失效
        reserved_     = nullptr;
        reserved_len_ = 0;
        return n;
    }

    /** @brief 自上次 Flush 以来已成功 TryWrite 的次数 */
    int Count() const noexcept { return count_; }

    /** @brief 自上次 Flush 以来已写入但尚未发布的字节数 */
    uint64_t PendingBytes() const noexcept { return w_ - w_start_; }

    /**
     * @brief 丢弃自上次 Flush 以来的所有未发布写入
     *
     * 未发布的数据对对端不可见（write_pos 未推进），丢弃是安全的。
     */
    void Cancel() noexcept
    {
        w_            = w_start_;
        count_        = 0;
        reserved_     = nullptr;
        reserved_len_ = 0;
    }

    /**
     * @brief 回滚到之前的 PendingBytes/Count 状态（撤销最近一次帧写入）
     * @param pending 之前通过 PendingBytes() 记录的值
     * @param count   之前通过 Count() 记录的值
     *
     * 一帧由多次 TryWrite 组成（header + 分段 payload），回滚必须同时
     * 恢复字节计数与写入次数计数；只减一次 count 会导致计数虚高。
     * 数据本身未发布，无需清除。
     */
    void RewindTo(uint64_t pending, int count) noexcept
    {
        if (!hdr_)
            return;  // moved-from
        // pending 超过当前未发布字节数会虚增 w_，
        // Flush 时把未写入的垃圾数据发布给对端。
        // assert 仅在 debug 生效，release 下钳制到实际未发布字节数。
        assert(pending <= PendingBytes());
        if (pending > PendingBytes())
            pending = PendingBytes();
        w_            = w_start_ + pending;
        count_        = (count < 0) ? 0 : count;
        reserved_     = nullptr;
        reserved_len_ = 0;
    }

    /** @brief 当前可写入的剩余字节数 */
    uint64_t FreeBytes() const noexcept { return Capacity - (w_ - r_); }

    /**
     * @brief 预留 len 字节连续写入区域，返回直写指针
     *
     * 仅当写入区域不跨越环尾时成功，返回指向 ring 数据区的指针。
     * 调用者必须在返回非空指针后自行填充数据，然后调用 CommitReserve(len)。
     * 空间不足或跨环尾时返回 nullptr，调用者应回退到 TryWrite。
     *
     * @param len 请求字节数
     * @return 非空 = 可直接写入的指针；nullptr = 不可用（跨环尾或空间不足）
     */
    char* Reserve(uint32_t len)
    {
        if (!hdr_ || !base_)  // moved-from / 已失效
            return nullptr;
        if (len == 0 || len > max_write_size)
            return nullptr;
        if ((w_ + len) - r_ > Capacity)
            return nullptr;
        std::size_t phys_w = RingPhys<Capacity>(w_);
        std::size_t tail   = Capacity - phys_w;
        if (!mirrored_ && len > tail)
            return nullptr;  // 非镜像映射跨环尾，调用者应回退
        reserved_     = base_ + phys_w;
        reserved_len_ = len;
        return reserved_;
    }

    /**
     * @brief 提交 Reserve 预留的写入
     * @param len 与 Reserve 调用时相同的长度
     *
     * release 构建同样校验：只有成功 Reserve 且长度一致才推进写位置，
     * 否则拒绝提交（不发布未写入的字节，避免破坏 w - r <= Capacity）。
     */
    void CommitReserve(uint32_t len)
    {
        // 注意：这里不做 assert。非法提交在 debug/release 下都应当被
        // 安全拒绝（而不是 abort），否则调试构建与发布构建行为不一致。
        if (!hdr_ || !reserved_ || len != reserved_len_)
            return;  // 非法提交：调用方未 Reserve 或长度不符
        w_ += len;
        ++count_;
        reserved_     = nullptr;
        reserved_len_ = 0;
    }

 private:
    RingHeader* hdr_  = nullptr;  ///< 为空表示惰性写入器
    char* base_       = nullptr;  ///< 数据区起始指针
    bool mirrored_    = false;    ///< 数据区是否做了镜像映射（可单次跨尾拷贝）
    uint64_t w_       = 0;        ///< 本地追踪的写位置（不触发原子 store）
    uint64_t r_       = 0;        ///< 快照的读位置（整批只读一次 acquire）
    uint64_t w_start_ = 0;        ///< Flush 前的写位置，用于判断是否有新数据
    int count_        = 0;        ///< 自上次 Flush 以来已写入次数计数

    // Reserve/CommitReserve 配对校验（release 下也生效）
    char* reserved_        = nullptr;  ///< 当前未提交的预留区起始指针
    uint32_t reserved_len_ = 0;        ///< 当前未提交的预留长度
};

}  // namespace detail

// ---------------------------------------------------------------------------
// RingBuf<Capacity>
// ---------------------------------------------------------------------------

/**
 * @brief 无锁 SPSC 字节流环形缓冲区
 *
 * 所有操作均为静态方法，接受原始共享内存指针。
 * 环形缓冲区为纯 POD 内存布局，完全位于共享内存中——无虚表、无堆、无跨进程指针。
 *
 * write_pos / read_pos 为单调递增字节偏移。
 * 物理偏移 = pos & (Capacity - 1)（Capacity 必须为 2 的幂）。
 *
 * @tparam Capacity 环形缓冲区容量（字节），必须为 2 的幂，默认 8MB
 */
template <std::size_t Capacity = 8 * 1024 * 1024>
class RingBuf
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 1024, "Capacity must be at least 1024 bytes");
    // len 参数与 Peek/TryRead 返回值均为 uint32_t，超出会静默截断
    static_assert(Capacity <= UINT32_MAX, "Capacity must fit in uint32_t (length fields are u32)");
    // 原子变量位于跨进程共享内存上，必须 lock-free
    // （否则可能退化为内部互斥锁，跨进程使用即死锁）
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "lock-free atomics are required for cross-process shm");

 public:
    static constexpr std::size_t capacity       = Capacity;
    static constexpr std::size_t shm_size       = kRingDataOffset + Capacity;
    static constexpr std::size_t max_write_size = Capacity / 2;

    /// @brief RAII 批量写入器（实现位于 detail 命名空间）
    using BatchWriter = detail::BatchWriter<Capacity>;

    // ---- 初始化 ----

    /**
     * @brief 初始化共享内存区域（清零）
     * @param shm 指向共享内存起始位置的指针
     *
     * @note 清零的可见性约定：Init 本身不做任何原子发布，write_pos/read_pos
     *       的初值（0）及数据区的清零对**对端进程**可见，依赖 memfd 通过
     *       SCM_RIGHTS（sendmsg/recvmsg）传递时内核提供的 happens-before：
     *       fd 传递是一次同步点，发送端在 sendmsg 前对该 mmap 区域的所有写入，
     *       在接收端 recvmsg 后必然可见。因此调用方必须保证 Init 在 fd 发送
     *       之前完成（见 RingChannel::CreateRingChannel 的调用顺序）。
     *       对端后续读到的 write_pos=0 即"环为空"，是安全的初态。
     */
    static void Init(void* shm)
    {
        // 控制头含 std::atomic：必须原地构造以开始对象生命周期，
        // 不能仅对原始 mmap 内存 memset（形式上 UB）。RingHeader{}
        // 的值初始化会把控制区清零，数据区单独清零。
        assert(reinterpret_cast<uintptr_t>(shm) % alignof(RingHeader) == 0);
        auto* hdr = new (shm) RingHeader{};
        hdr->write_pos.store(0, std::memory_order_relaxed);
        hdr->read_pos.store(0, std::memory_order_relaxed);
        // 读方尚未声明支持按需通知前，写方保持"每次都通知"，兼容不调用
        // ArmReaderWait 的裸读方
        hdr->reader_wait.store(kReaderNotifyAlways, std::memory_order_relaxed);
        // 清零 header 之后的填充页与数据区
        std::memset(static_cast<char*>(shm) + sizeof(RingHeader), 0, shm_size - sizeof(RingHeader));
    }

    // ---- 写入 ----

    /**
     * @brief 尝试写入字节数据，缓冲区空间不足或数据过大时返回 -1
     *
     * 数据跨越环尾时自动拆分为两段 memcpy。
     *
     * @param shm  共享内存指针
     * @param data 待写入数据
     * @param len  数据长度（字节）
     * @return 0 成功，-1 失败
     */
    static int TryWrite(void* shm, const void* data, uint32_t len)
    {
        if (len == 0 || len > max_write_size)
            return -1;
        if (data == nullptr)  // memcpy(dst, nullptr, n) 是 UB，防御性拒绝
            return -1;

        auto* hdr  = detail::RingHdr(shm);
        char* base = detail::RingData(shm);

        uint64_t w = hdr->write_pos.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);

        if ((w + len) - r > Capacity)
            return -1;

        std::size_t phys_w = detail::RingPhys<Capacity>(w);
        std::size_t tail   = Capacity - phys_w;

        auto* src = static_cast<const char*>(data);
        if (len <= tail)
        {
            std::memcpy(base + phys_w, src, len);
        }
        else
        {
            std::memcpy(base + phys_w, src, tail);
            std::memcpy(base, src + tail, len - tail);
        }

        hdr->write_pos.store(w + len, std::memory_order_release);
        return 0;
    }

    // ---- 读取 ----

    /**
     * @brief 读取最多 max_len 字节，缓冲区空时返回 0
     *
     * 数据跨越环尾时自动拆分为两段 memcpy。
     *
     * @param shm     共享内存指针
     * @param data    输出缓冲区
     * @param max_len 最大读取字节数
     * @return 实际读取的字节数（0 表示缓冲区为空）
     */
    static uint32_t TryRead(void* shm, void* data, uint32_t max_len)
    {
        // memcpy(dst, src, n>0) 对空指针属 UB；与 TryWrite 的防御保持一致
        if (data == nullptr)
            return 0;

        auto* hdr        = detail::RingHdr(shm);
        const char* base = detail::RingData(shm);

        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w     = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail = w - r;

        if (avail == 0)
            return 0;

        uint32_t to_read = (avail < max_len) ? static_cast<uint32_t>(avail) : max_len;
        // max_len == 0：不拷贝不推进
        if (to_read == 0)
            return 0;
        detail::RingCopyOut<Capacity>(base, detail::RingPhys<Capacity>(r), data, to_read);

        hdr->read_pos.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    /**
     * @brief 精确读取 len 字节，数据不足时返回 -1 且不消费任何字节
     * @param shm  共享内存指针
     * @param data 输出缓冲区（至少 len 字节）
     * @param len  需要读取的精确字节数
     * @return 0 成功，-1 数据不足
     */
    static int ReadExact(void* shm, void* data, uint32_t len)
    {
        // 读 0 字节平凡成功；提前返回避免 memcpy(dst, src, 0) 的 UB 边界
        if (len == 0)
            return 0;
        if (data == nullptr)  // 非零长度读入空指针：拒绝而不是崩溃
            return -1;

        auto* hdr        = detail::RingHdr(shm);
        const char* base = detail::RingData(shm);

        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w     = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail = w - r;

        if (avail < len)
            return -1;

        detail::RingCopyOut<Capacity>(base, detail::RingPhys<Capacity>(r), data, len);

        hdr->read_pos.store(r + len, std::memory_order_release);
        return 0;
    }

    // ---- 镜像映射专用（数据区连续两圈，可单次跨尾拷贝）----
    // 这些方法要求 shm 通过 RingChannel 的镜像映射建立；普通 mmap/
    // 非镜像 RingBuf 请使用上面的 TryWrite/TryRead/ReadExact/Peek。

    /// @brief 镜像映射下写入任意长度（单次 memcpy，可跨环尾）
    static int TryWriteContig(void* shm, const void* data, uint32_t len)
    {
        if (len == 0 || len > max_write_size || data == nullptr)
            return -1;

        auto* hdr  = detail::RingHdr(shm);
        char* base = detail::RingData(shm);
        uint64_t w = hdr->write_pos.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);
        if ((w + len) - r > Capacity)
            return -1;

        std::memcpy(base + detail::RingPhys<Capacity>(w), data, len);
        hdr->write_pos.store(w + len, std::memory_order_release);
        return 0;
    }

    /// @brief 镜像映射下读取最多 max_len 字节（单次 memcpy）
    static uint32_t TryReadContig(void* shm, void* data, uint32_t max_len)
    {
        if (data == nullptr)
            return 0;

        auto* hdr        = detail::RingHdr(shm);
        const char* base = detail::RingData(shm);
        uint64_t r       = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w       = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail   = w - r;
        if (avail == 0)
            return 0;

        uint32_t to_read = (avail < max_len) ? static_cast<uint32_t>(avail) : max_len;
        if (to_read == 0)
            return 0;

        std::memcpy(data, base + detail::RingPhys<Capacity>(r), to_read);
        hdr->read_pos.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    /// @brief 镜像映射下精确读取 len 字节（单次 memcpy）
    static int ReadExactContig(void* shm, void* data, uint32_t len)
    {
        if (len == 0)
            return 0;
        if (data == nullptr)
            return -1;

        auto* hdr        = detail::RingHdr(shm);
        const char* base = detail::RingData(shm);
        uint64_t r       = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w       = hdr->write_pos.load(std::memory_order_acquire);
        if (w - r < len)
            return -1;

        std::memcpy(data, base + detail::RingPhys<Capacity>(r), len);
        hdr->read_pos.store(r + len, std::memory_order_release);
        return 0;
    }

    /// @brief 镜像映射下单段 Peek（永远返回一段，最多 Capacity 字节）
    static int PeekContig(const void* shm, const void** data, uint32_t* len)
    {
        if (!data || !len)
            return -1;

        auto* hdr        = detail::RingHdr(shm);
        const char* base = detail::RingData(shm);
        uint64_t r       = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w       = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail   = w - r;
        if (avail == 0)
            return -1;

        *data = base + detail::RingPhys<Capacity>(r);
        *len  = static_cast<uint32_t>(avail);
        return 0;
    }

    /**
     * @brief 两段式零拷贝 Peek：返回可读数据的一或两个连续段指针
     *
     * 当可读数据跨越环尾回绕时，seg1 指向尾部数据，seg2 指向头部数据。
     * 未回绕时 seg2_len 为 0。调用 CommitRead 前指针有效。
     *
     * @param shm      共享内存指针（const）
     * @param seg1     输出：第一段数据指针
     * @param seg1_len 输出：第一段长度
     * @param seg2     输出：第二段数据指针（回绕部分，可能为 nullptr）
     * @param seg2_len 输出：第二段长度（可能为 0）
     * @return 0 有数据，-1 缓冲区为空
     */
    static int Peek(const void* shm, const void** seg1, uint32_t* seg1_len, const void** seg2, uint32_t* seg2_len)
    {
        // 输出参数为空时安全拒绝（debug/release 行为一致）
        if (!seg1 || !seg1_len || !seg2 || !seg2_len)
            return -1;

        auto* hdr        = detail::RingHdr(shm);
        const char* base = detail::RingData(shm);

        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w     = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail = w - r;

        if (avail == 0)
            return -1;

        std::size_t phys_r = detail::RingPhys<Capacity>(r);
        std::size_t contig = Capacity - phys_r;

        if (avail <= contig)
        {
            *seg1     = base + phys_r;
            *seg1_len = static_cast<uint32_t>(avail);
            *seg2     = nullptr;
            *seg2_len = 0;
        }
        else
        {
            *seg1     = base + phys_r;
            *seg1_len = static_cast<uint32_t>(contig);
            *seg2     = base;
            *seg2_len = static_cast<uint32_t>(avail - contig);
        }
        return 0;
    }

    /**
     * @brief 提交读取：推进 read_pos
     * @param shm 共享内存指针
     * @param len 要推进的字节数（不得超过当前可读字节数）
     *
     * @note len 超过可读字节数会破坏 r <= w 不变式。debug 构建触发
     *       assert；release 构建钳制到实际可读字节数，避免静默损坏。
     */
    static void CommitRead(void* shm, uint32_t len)
    {
        auto* hdr      = detail::RingHdr(shm);
        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t avail = hdr->write_pos.load(std::memory_order_acquire) - r;
        assert(static_cast<uint64_t>(len) <= avail);
        if (static_cast<uint64_t>(len) > avail)
            len = static_cast<uint32_t>(avail);  // release 防御：不越过写位置
        hdr->read_pos.store(r + len, std::memory_order_release);
    }

    /**
     * @brief 返回环中可读字节数
     * @param shm 共享内存指针（const）
     * @return 可读字节数
     */
    static uint64_t Available(const void* shm)
    {
        auto* hdr  = detail::RingHdr(shm);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);
        return w - r;
    }

    /** @brief 读取 write_pos（acquire） */
    static uint64_t WritePos(const void* shm)
    {
        return detail::RingHdr(shm)->write_pos.load(std::memory_order_acquire);
    }

    /** @brief 读取 read_pos（acquire） */
    static uint64_t ReadPos(const void* shm) { return detail::RingHdr(shm)->read_pos.load(std::memory_order_acquire); }

    // ---- 按需通知（reader_wait）----

    /**
     * @brief 读方：声明即将睡眠，并复查是否有新数据（读方调用）
     *
     * 流程：置 kReaderWaiting → seq_cst 栅栏 → 复查 write_pos。
     * 与写方 ShouldNotify 的"发布 write_pos → seq_cst 栅栏 → 读标志"构成
     * Dekker 式配对：两端至少有一方能看到对方的写入，因此不会丢唤醒。
     * 首次调用即表示该读方启用按需通知（标志离开 kReaderNotifyAlways）。
     *
     * @param shm        读环共享内存指针
     * @param seen_write 读方上次处理数据时观察到的 write_pos 基线
     *                   （空环判断可传当前 read_pos）
     * @return true 无新数据，可以睡眠；false write_pos 已变化，
     *         已撤销标志，调用方应继续读取而不要睡眠
     */
    static bool ArmReaderWait(void* shm, uint64_t seen_write)
    {
        auto* hdr = detail::RingHdr(shm);
        hdr->reader_wait.store(kReaderWaiting, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (hdr->write_pos.load(std::memory_order_acquire) != seen_write)
        {
            // 若写方已抢先清零并通知，这里覆盖为 Awake 无副作用（仅多一次空唤醒）
            hdr->reader_wait.store(kReaderAwake, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    /**
     * @brief 写方：发布数据后判断是否需要通知对端（写方调用）
     *
     * 必须在 write_pos 发布之后调用。读方处于 kReaderWaiting 时，本函数
     * 原子地清零标志并返回 true（每次睡眠周期仅一次通知）；读方在处理中
     * 返回 false；读方未启用按需通知（kReaderNotifyAlways）恒返回 true。
     *
     * @param shm 写环共享内存指针
     */
    static bool ShouldNotify(void* shm)
    {
        auto* hdr = detail::RingHdr(shm);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint32_t v = hdr->reader_wait.load(std::memory_order_relaxed);
        if (v == kReaderAwake)
            return false;
        if (v == kReaderNotifyAlways)
            return true;
        // kReaderWaiting：抢占清零，只有抢到的一方发通知
        return hdr->reader_wait.exchange(kReaderAwake, std::memory_order_acq_rel) != kReaderAwake;
    }

    /**
     * @brief 返回环中剩余可写字节数
     * @param shm 共享内存指针（const）
     * @return 可写字节数
     */
    static uint64_t FreeSpace(const void* shm) { return Capacity - Available(shm); }
};

/// @brief 默认配置别名（8MB 环）
using DefaultRingBuf = RingBuf<>;

}  // namespace shm

#endif  // SHM_IPC_RINGBUF_HPP_
