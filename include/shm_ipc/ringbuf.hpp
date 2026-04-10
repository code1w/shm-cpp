/**
 * @file ringbuf.hpp
 * @brief 字节级无锁 SPSC 环形缓冲区（C++17）
 *
 * 支持可变长度消息，最大 ~4MB（Capacity/2 - 8 字节）。
 * 消息带长度前缀，在连续字节环中以 8 字节对齐存储。
 *
 * 内存布局：
 * @code
 * ┌──────────────────────────────────────────────┐
 * │ RingHeader（控制区，缓存行对齐填充）          │
 * ├──────────────────────────────────────────────┤
 * │ [MsgHdr][payload..][pad] [MsgHdr][payload..] │
 * │ [pad] [MsgHdr][payload..][pad] ...           │
 * │         （连续字节环）                        │
 * └──────────────────────────────────────────────┘
 * @endcode
 *
 * 消息帧格式：
 * @code
 * ┌────────┬────────┬─────────────────┬─────────┐
 * │ len u32│ seq u32│ payload (len B) │ pad to 8│
 * └────────┴────────┴─────────────────┴─────────┘
 * @endcode
 *
 * 回绕处理：当尾部剩余连续空间不足一个帧时，写入哨兵（len = UINT32_MAX）
 * 标记"跳转到偏移 0"。8 字节对齐保证哨兵头总能写入。
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
struct RingHeader
{
    std::atomic<uint64_t> write_pos;  ///< 写位置（单调递增字节偏移）
    char _pad1[56];                   ///< 填充至 64 字节（一个缓存行）
    std::atomic<uint64_t> read_pos;   ///< 读位置（单调递增字节偏移）
    char _pad2[56];                   ///< 填充至 64 字节
};

/** @brief 消息帧头，紧跟 payload */
struct MsgHeader
{
    uint32_t len;  ///< payload 长度（字节），UINT32_MAX 表示哨兵
    uint32_t seq;  ///< 消息序列号
};

static_assert(sizeof(MsgHeader) == 8, "MsgHeader must be 8 bytes");

// ---------------------------------------------------------------------------
// RingBuf<Capacity>
// ---------------------------------------------------------------------------

/**
 * @brief 无锁 SPSC 环形缓冲区
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
    static constexpr std::size_t capacity        = Capacity;
    static constexpr std::size_t shm_size        = sizeof(RingHeader) + Capacity;
    static constexpr std::size_t msg_header_size = sizeof(MsgHeader);
    static constexpr std::size_t max_msg_size    = Capacity / 2 - msg_header_size;

    static constexpr uint32_t kSentinel = UINT32_MAX;  ///< 回绕哨兵标记

    /**
     * @brief 将（头 + payload）向上对齐到 8 字节
     * @param len payload 长度
     * @return 帧总大小（字节）
     */
    static constexpr std::size_t FrameSize(uint32_t len)
    {
        return (msg_header_size + len + 7) & ~std::size_t(7);
    }

    // ---- 初始化 ----

    /**
     * @brief 初始化共享内存区域（清零）
     * @param shm 指向共享内存起始位置的指针
     */
    static void Init(void *shm) { std::memset(shm, 0, shm_size); }

    // ---- 写入 ----

    /**
     * @brief 尝试写入可变长消息，缓冲区满或消息过大时返回 -1
     * @param shm  共享内存指针
     * @param data payload 数据
     * @param len  payload 长度
     * @param seq  消息序列号
     * @return 0 成功，-1 失败
     */
    static int TryWrite(void *shm, const void *data, uint32_t len, uint32_t seq)
    {
        if (len > max_msg_size)
            return -1;

        auto *hdr  = Header(shm);
        char *base = DataRegion(shm);

        uint64_t w         = hdr->write_pos.load(std::memory_order_relaxed);
        uint64_t r         = hdr->read_pos.load(std::memory_order_acquire);
        std::size_t total  = FrameSize(len);
        std::size_t phys_w = Mask(w);
        std::size_t tail   = Capacity - phys_w;

        if (total > tail)
        {
            // 需要哨兵回绕：tail 字节写哨兵 + total 字节从偏移 0 写正文
            if ((w + tail + total) - r > Capacity)
                return -1;

            auto *sentinel = reinterpret_cast<MsgHeader *>(base + phys_w);
            sentinel->len  = kSentinel;
            sentinel->seq  = 0;
            w += tail;

            auto *mh = reinterpret_cast<MsgHeader *>(base);
            mh->len  = len;
            mh->seq  = seq;
            std::memcpy(base + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        }
        else
        {
            if ((w + total) - r > Capacity)
                return -1;

            auto *mh = reinterpret_cast<MsgHeader *>(base + phys_w);
            mh->len  = len;
            mh->seq  = seq;
            std::memcpy(base + phys_w + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        }
        return 0;
    }

    /**
     * @brief 强制写入：缓冲区满时丢弃最旧消息
     *
     * **注意**：此函数从写方推进 read_pos，使用 compare_exchange 避免
     * 与读方的 store 竞争。仅在读方可能已消费更多数据时让步（CAS 失败
     * 意味着读方已推进 read_pos，重新计算空间即可）。
     *
     * @param shm  共享内存指针
     * @param data payload 数据
     * @param len  payload 长度
     * @param seq  消息序列号
     */
    static void ForceWrite(void *shm, const void *data, uint32_t len,
                           uint32_t seq)
    {
        if (len > max_msg_size)
            return;

        auto *hdr  = Header(shm);
        char *base = DataRegion(shm);

        uint64_t w         = hdr->write_pos.load(std::memory_order_relaxed);
        std::size_t total  = FrameSize(len);

    retry:
        uint64_t r         = hdr->read_pos.load(std::memory_order_acquire);
        std::size_t phys_w = Mask(w);
        std::size_t tail   = Capacity - phys_w;
        bool need_sentinel = (total > tail);
        std::size_t cost   = need_sentinel ? (tail + total) : total;

        // 丢弃最旧消息直到有足够空间
        uint64_t new_r = r;
        while ((w + cost) - new_r > Capacity)
        {
            std::size_t phys_r = Mask(new_r);
            auto *mh           = reinterpret_cast<const MsgHeader *>(base + phys_r);
            if (mh->len == kSentinel)
                new_r += Capacity - phys_r;
            else
                new_r += FrameSize(mh->len);
        }
        // CAS 推进 read_pos：如果读方已推进得更远，CAS 失败后重试
        if (new_r != r)
        {
            if (!hdr->read_pos.compare_exchange_strong(
                    r, new_r, std::memory_order_release, std::memory_order_acquire))
                goto retry;  // 读方已推进，重新计算空间
        }

        // 保证空间后写入
        if (need_sentinel)
        {
            auto *sentinel = reinterpret_cast<MsgHeader *>(base + phys_w);
            sentinel->len  = kSentinel;
            sentinel->seq  = 0;
            w += tail;

            auto *mh = reinterpret_cast<MsgHeader *>(base);
            mh->len  = len;
            mh->seq  = seq;
            std::memcpy(base + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        }
        else
        {
            auto *mh = reinterpret_cast<MsgHeader *>(base + phys_w);
            mh->len  = len;
            mh->seq  = seq;
            std::memcpy(base + phys_w + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        }
    }

    // ---- 读取 ----

    /**
     * @brief 尝试读取一条可变长消息，缓冲区空时返回 -1
     * @param shm  共享内存指针
     * @param data 输出缓冲区，至少 max_msg_size 字节
     * @param len  输出：payload 长度
     * @param seq  输出：消息序列号
     * @return 0 成功，-1 缓冲区为空
     */
    static int TryRead(void *shm, void *data, uint32_t *len, uint32_t *seq)
    {
        auto *hdr        = Header(shm);
        const char *base = DataRegion(shm);

        uint64_t r = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);

        if (r >= w)
            return -1;

        std::size_t phys_r = Mask(r);
        auto *mh           = reinterpret_cast<const MsgHeader *>(base + phys_r);

        // 遇到哨兵则跳转到偏移 0
        if (mh->len == kSentinel)
        {
            r += Capacity - phys_r;
            if (r >= w)
                return -1;

            phys_r = Mask(r);  // 此时应为 0
            mh     = reinterpret_cast<const MsgHeader *>(base + phys_r);
        }

        uint32_t payload_len = mh->len;
        if (payload_len > max_msg_size)
            return -1;  // 防御性检查：损坏的 shm 数据
        if (seq)
            *seq = mh->seq;
        if (len)
            *len = payload_len;
        if (data)
            std::memcpy(data, base + phys_r + msg_header_size, payload_len);

        hdr->read_pos.store(r + FrameSize(payload_len), std::memory_order_release);
        return 0;
    }

    /**
     * @brief 零拷贝读取：返回指向环形缓冲区内部的指针，无需复制
     *
     * 返回的指针在调用 CommitRead() 之前有效。
     * @param shm  共享内存指针
     * @param data 输出：指向 payload 的指针
     * @param len  输出：payload 长度
     * @param seq  输出：消息序列号
     * @return 0 成功，-1 缓冲区为空
     */
    static int PeekRead(void *shm, const void **data, uint32_t *len,
                        uint32_t *seq)
    {
        auto *hdr        = Header(shm);
        const char *base = DataRegion(shm);

        uint64_t r = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);

        if (r >= w)
            return -1;

        std::size_t phys_r = Mask(r);
        auto *mh           = reinterpret_cast<const MsgHeader *>(base + phys_r);

        if (mh->len == kSentinel)
        {
            r += Capacity - phys_r;
            if (r >= w)
                return -1;
            phys_r = Mask(r);
            mh     = reinterpret_cast<const MsgHeader *>(base + phys_r);
        }

        if (mh->len > max_msg_size)
            return -1;  // 防御性检查：损坏的 shm 数据

        if (len)
            *len = mh->len;
        if (seq)
            *seq = mh->seq;
        if (data)
            *data = base + phys_r + msg_header_size;
        return 0;
    }

    /**
     * @brief 提交上一次 PeekRead，推进 read_pos
     * @param shm 共享内存指针
     * @param len PeekRead 返回的 payload 长度
     */
    static void CommitRead(void *shm, uint32_t len)
    {
        auto *hdr        = Header(shm);
        const char *base = DataRegion(shm);

        uint64_t r         = hdr->read_pos.load(std::memory_order_relaxed);
        std::size_t phys_r = Mask(r);
        auto *mh           = reinterpret_cast<const MsgHeader *>(base + phys_r);

        // 如遇哨兵则先跳过（与 PeekRead 逻辑一致）
        if (mh->len == kSentinel)
            r += Capacity - phys_r;

        hdr->read_pos.store(r + FrameSize(len), std::memory_order_release);
    }

    /**
     * @brief 返回环中已使用字节数（含帧头、填充、哨兵）
     * @param shm 共享内存指针（const）
     * @return 已使用字节数
     */
    static uint64_t Available(const void *shm)
    {
        auto *hdr  = Header(shm);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);
        return w - r;
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
};

/// @brief 默认配置别名（8MB 环，~4MB 最大消息）
using DefaultRingBuf = RingBuf<>;

}  // namespace shm_ipc

#endif  // SHM_IPC_RINGBUF_HPP_
