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
#include <cstdint>
#include <cstring>

namespace shm_ipc {

// ---------------------------------------------------------------------------
// 共享内存结构（POD，无指针）
// ---------------------------------------------------------------------------

/** @brief 环形缓冲区控制头，写/读位置各占独立缓存行 */
struct alignas(64) RingHeader
{
    std::atomic<uint64_t> write_pos;  ///< 写位置（单调递增字节偏移）
    char pad1[64 - sizeof(std::atomic<uint64_t>)];  ///< 填充至 64 字节（一个缓存行）
    std::atomic<uint64_t> read_pos;   ///< 读位置（单调递增字节偏移）
    char pad2[64 - sizeof(std::atomic<uint64_t>)];  ///< 填充至 64 字节
};

static_assert(sizeof(RingHeader) == 128, "RingHeader must be 128 bytes (2 cache lines)");
static_assert(alignof(RingHeader) == 64, "RingHeader must be aligned to 64 bytes");

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
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(Capacity >= 1024, "Capacity must be at least 1024 bytes");

 public:
    static constexpr std::size_t capacity       = Capacity;
    static constexpr std::size_t shm_size       = sizeof(RingHeader) + Capacity;
    static constexpr std::size_t max_write_size = Capacity / 2;

    // ---- 初始化 ----

    /**
     * @brief 初始化共享内存区域（清零）
     * @param shm 指向共享内存起始位置的指针
     */
    static void Init(void *shm) { std::memset(shm, 0, shm_size); }

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
    static int TryWrite(void *shm, const void *data, uint32_t len)
    {
        if (len == 0 || len > max_write_size)
            return -1;

        auto *hdr  = Header(shm);
        char *base = DataRegion(shm);

        uint64_t w = hdr->write_pos.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);

        if ((w + len) - r > Capacity)
            return -1;

        std::size_t phys_w = Mask(w);
        std::size_t tail   = Capacity - phys_w;

        auto *src = static_cast<const char *>(data);
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

    // ---- 批量写入 ----

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
     */
    class BatchWriter
    {
     public:
        /**
         * @brief 构造批量写入器，快照当前 write_pos 和 read_pos
         * @param shm 共享内存指针
         */
        explicit BatchWriter(void *shm)
            : hdr_(Header(shm)), base_(DataRegion(shm)),
              w_(hdr_->write_pos.load(std::memory_order_relaxed)),
              r_(hdr_->read_pos.load(std::memory_order_acquire)),
              w_start_(w_) {}

        ~BatchWriter() { Flush(); }

        BatchWriter(BatchWriter &&o) noexcept
            : hdr_(o.hdr_), base_(o.base_),
              w_(o.w_), r_(o.r_), w_start_(o.w_start_),
              count_(o.count_)
        {
            o.hdr_ = nullptr;
        }

        BatchWriter &operator=(BatchWriter &&)      = delete;
        BatchWriter(const BatchWriter &)            = delete;
        BatchWriter &operator=(const BatchWriter &) = delete;

        /**
         * @brief 尝试写入一段数据到批量缓冲区（不触发原子 store）
         * @param data 待写入数据
         * @param len  数据长度（字节）
         * @return 0 成功，-1 空间不足或数据过大
         */
        int TryWrite(const void *data, uint32_t len)
        {
            if (len == 0 || len > max_write_size)
                return -1;

            if ((w_ + len) - r_ > Capacity)
                return -1;

            std::size_t phys_w = Mask(w_);
            std::size_t tail   = Capacity - phys_w;

            auto *src = static_cast<const char *>(data);
            if (len <= tail)
            {
                std::memcpy(base_ + phys_w, src, len);
            }
            else
            {
                std::memcpy(base_ + phys_w, src, tail);
                std::memcpy(base_, src + tail, len - tail);
            }

            w_ += len;
            ++count_;
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
            return n;
        }

        /** @brief 自上次 Flush 以来已成功 TryWrite 的次数 */
        int Count() const noexcept { return count_; }

        /** @brief 当前可写入的剩余字节数 */
        uint64_t FreeBytes() const noexcept { return Capacity - (w_ - r_); }

     private:
        RingHeader *hdr_;
        char *base_;
        uint64_t w_;        ///< 本地追踪的写位置（不触发原子 store）
        uint64_t r_;        ///< 快照的读位置（整批只读一次 acquire）
        uint64_t w_start_;  ///< Flush 前的写位置，用于判断是否有新数据
        int count_ = 0;     ///< 自上次 Flush 以来已写入次数计数
    };

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
    static uint32_t TryRead(void *shm, void *data, uint32_t max_len)
    {
        auto *hdr        = Header(shm);
        const char *base = DataRegion(shm);

        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w     = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail = w - r;

        if (avail == 0)
            return 0;

        uint32_t to_read = (avail < max_len) ? static_cast<uint32_t>(avail) : max_len;
        CopyOut(base, Mask(r), data, to_read);

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
    static int ReadExact(void *shm, void *data, uint32_t len)
    {
        auto *hdr        = Header(shm);
        const char *base = DataRegion(shm);

        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w     = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail = w - r;

        if (avail < len)
            return -1;

        CopyOut(base, Mask(r), data, len);

        hdr->read_pos.store(r + len, std::memory_order_release);
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
    static int Peek(const void *shm, const void **seg1, uint32_t *seg1_len,
                    const void **seg2, uint32_t *seg2_len)
    {
        auto *hdr        = Header(shm);
        const char *base = DataRegion(shm);

        uint64_t r     = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w     = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t avail = w - r;

        if (avail == 0)
            return -1;

        std::size_t phys_r = Mask(r);
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
     * @param len 要推进的字节数
     */
    static void CommitRead(void *shm, uint32_t len)
    {
        auto *hdr = Header(shm);
        uint64_t r = hdr->read_pos.load(std::memory_order_relaxed);
        hdr->read_pos.store(r + len, std::memory_order_release);
    }

    /**
     * @brief 返回环中可读字节数
     * @param shm 共享内存指针（const）
     * @return 可读字节数
     */
    static uint64_t Available(const void *shm)
    {
        auto *hdr  = Header(shm);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);
        return w - r;
    }

    /**
     * @brief 返回环中剩余可写字节数
     * @param shm 共享内存指针（const）
     * @return 可写字节数
     */
    static uint64_t FreeSpace(const void *shm)
    {
        return Capacity - Available(shm);
    }

 private:
    static RingHeader *Header(void *shm)
    {
        return static_cast<RingHeader *>(shm);
    }
    static const RingHeader *Header(const void *shm)
    {
        return static_cast<const RingHeader *>(shm);
    }
    static char *DataRegion(void *shm)
    {
        return static_cast<char *>(shm) + sizeof(RingHeader);
    }
    static const char *DataRegion(const void *shm)
    {
        return static_cast<const char *>(shm) + sizeof(RingHeader);
    }
    static constexpr std::size_t Mask(uint64_t pos)
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
    static void CopyOut(const char *base, std::size_t phys_r,
                        void *dst, uint32_t len)
    {
        std::size_t contig = Capacity - phys_r;
        auto *d = static_cast<char *>(dst);
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
};

/// @brief 默认配置别名（8MB 环）
using DefaultRingBuf = RingBuf<>;

}  // namespace shm_ipc

#endif  // SHM_IPC_RINGBUF_HPP_
