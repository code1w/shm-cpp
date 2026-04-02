// ringbuf.hpp — Byte-oriented lock-free SPSC ring buffer (C++17)
//
// Supports variable-length messages up to ~4MB (Capacity/2 - 8 bytes).
// Messages are length-prefixed and 8-byte aligned in a contiguous byte ring.
//
// Memory layout:
// ┌──────────────────────────────────────────────┐
// │ RingHeader (control, cache-line padded)       │
// ├──────────────────────────────────────────────┤
// │ [MsgHdr][payload..][pad] [MsgHdr][payload..] │
// │ [pad] [MsgHdr][payload..][pad] ...           │
// │         (contiguous byte ring)                │
// └──────────────────────────────────────────────┘
//
// Message frame:
// ┌────────┬────────┬─────────────────┬─────────┐
// │ len u32│ seq u32│ payload (len B) │ pad to 8│
// └────────┴────────┴─────────────────┴─────────┘
//
// Wrap handling: when contiguous tail space < frame_size, a sentinel
// (len = UINT32_MAX) is written to mark "skip to offset 0".
// 8-byte alignment guarantees the sentinel header always fits.

#ifndef SHM_IPC_RINGBUF_HPP
#define SHM_IPC_RINGBUF_HPP

#include <atomic>
#include <cstdint>
#include <cstring>

namespace shm_ipc {

// ---------------------------------------------------------------------------
// Shared-memory structures (POD, no pointers)
// ---------------------------------------------------------------------------

struct RingHeader {
    std::atomic<uint64_t> write_pos;
    char _pad1[56];
    std::atomic<uint64_t> read_pos;
    char _pad2[56];
};

struct MsgHeader {
    uint32_t len;
    uint32_t seq;
};

static_assert(sizeof(MsgHeader) == 8, "MsgHeader must be 8 bytes");

// ---------------------------------------------------------------------------
// RingBuf<Capacity>
//
// All operations are static and take a raw shm pointer.  The ring buffer
// is a pure POD memory layout living entirely in shared memory — no vtable,
// no heap, no cross-process pointers.
//
// write_pos / read_pos are monotonically increasing byte offsets.
// Physical offset = pos & (Capacity - 1)  (Capacity must be power of 2).
// ---------------------------------------------------------------------------

template <std::size_t Capacity = 8 * 1024 * 1024>
class RingBuf {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(Capacity >= 1024,
                  "Capacity must be at least 1024 bytes");

public:
    static constexpr std::size_t capacity        = Capacity;
    static constexpr std::size_t shm_size        = sizeof(RingHeader) + Capacity;
    static constexpr std::size_t msg_header_size = sizeof(MsgHeader);
    static constexpr std::size_t max_msg_size    = Capacity / 2 - msg_header_size;

    static constexpr uint32_t kSentinel = UINT32_MAX;

    /// Round (header + payload) up to 8-byte alignment.
    static constexpr std::size_t frame_size(uint32_t len) {
        return (msg_header_size + len + 7) & ~std::size_t(7);
    }

    // ---- init ----

    static void init(void *shm) {
        std::memset(shm, 0, shm_size);
    }

    // ---- write ----

    /// Try to write a variable-length message.  Returns -1 if full or too large.
    static int try_write(void *shm, const void *data, uint32_t len, uint32_t seq) {
        if (len > max_msg_size) return -1;

        auto *hdr = header(shm);
        char *base = data_region(shm);

        uint64_t w = hdr->write_pos.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);

        std::size_t total = frame_size(len);
        std::size_t phys_w = mask(w);
        std::size_t tail = Capacity - phys_w;

        if (total > tail) {
            // Need sentinel wrap: tail bytes for sentinel + total bytes at offset 0
            if ((w + tail + total) - r > Capacity) return -1;

            // Write sentinel at current position
            auto *sentinel = reinterpret_cast<MsgHeader *>(base + phys_w);
            sentinel->len = kSentinel;
            sentinel->seq = 0;

            w += tail; // advance past sentinel region, now phys = 0

            // Write real message at offset 0
            auto *mh = reinterpret_cast<MsgHeader *>(base);
            mh->len = len;
            mh->seq = seq;
            std::memcpy(base + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        } else {
            if ((w + total) - r > Capacity) return -1;

            auto *mh = reinterpret_cast<MsgHeader *>(base + phys_w);
            mh->len = len;
            mh->seq = seq;
            std::memcpy(base + phys_w + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        }
        return 0;
    }

    /// Force write — discards oldest messages when the buffer is full.
    static void force_write(void *shm, const void *data, uint32_t len, uint32_t seq) {
        if (len > max_msg_size) return;

        auto *hdr = header(shm);
        char *base = data_region(shm);

        uint64_t w = hdr->write_pos.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);

        std::size_t total = frame_size(len);
        std::size_t phys_w = mask(w);
        std::size_t tail = Capacity - phys_w;

        bool need_sentinel = (total > tail);
        std::size_t cost = need_sentinel ? (tail + total) : total;

        // Discard oldest messages until we have room
        while ((w + cost) - r > Capacity) {
            std::size_t phys_r = mask(r);
            auto *mh = reinterpret_cast<const MsgHeader *>(base + phys_r);
            if (mh->len == kSentinel) {
                r += Capacity - phys_r; // skip sentinel region
            } else {
                r += frame_size(mh->len);
            }
        }
        hdr->read_pos.store(r, std::memory_order_release);

        // Now write (guaranteed space)
        if (need_sentinel) {
            auto *sentinel = reinterpret_cast<MsgHeader *>(base + phys_w);
            sentinel->len = kSentinel;
            sentinel->seq = 0;
            w += tail;

            auto *mh = reinterpret_cast<MsgHeader *>(base);
            mh->len = len;
            mh->seq = seq;
            std::memcpy(base + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        } else {
            auto *mh = reinterpret_cast<MsgHeader *>(base + phys_w);
            mh->len = len;
            mh->seq = seq;
            std::memcpy(base + phys_w + msg_header_size, data, len);

            hdr->write_pos.store(w + total, std::memory_order_release);
        }
    }

    // ---- read ----

    /// Try to read one variable-length message.  Returns -1 when empty.
    /// `data` must point to a buffer of at least `max_msg_size` bytes (or the
    /// caller must know the upper bound of incoming message sizes).
    static int try_read(void *shm, void *data, uint32_t *len, uint32_t *seq) {
        auto *hdr = header(shm);
        const char *base = data_region(shm);

        uint64_t r = hdr->read_pos.load(std::memory_order_relaxed);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);

        if (r >= w) return -1;

        std::size_t phys_r = mask(r);
        auto *mh = reinterpret_cast<const MsgHeader *>(base + phys_r);

        // Skip sentinel if present
        if (mh->len == kSentinel) {
            r += Capacity - phys_r;
            if (r >= w) return -1; // empty after skipping sentinel

            phys_r = mask(r); // should be 0
            mh = reinterpret_cast<const MsgHeader *>(base + phys_r);
        }

        uint32_t payload_len = mh->len;
        if (seq) *seq = mh->seq;
        if (len) *len = payload_len;
        if (data) std::memcpy(data, base + phys_r + msg_header_size, payload_len);

        hdr->read_pos.store(r + frame_size(payload_len), std::memory_order_release);
        return 0;
    }

    /// Bytes used in the ring (including headers, padding, sentinels).
    static uint64_t available(const void *shm) {
        auto *hdr = header(shm);
        uint64_t w = hdr->write_pos.load(std::memory_order_acquire);
        uint64_t r = hdr->read_pos.load(std::memory_order_acquire);
        return w - r;
    }

private:
    static RingHeader *header(void *shm) {
        return static_cast<RingHeader *>(shm);
    }
    static const RingHeader *header(const void *shm) {
        return static_cast<const RingHeader *>(shm);
    }
    static char *data_region(void *shm) {
        return static_cast<char *>(shm) + sizeof(RingHeader);
    }
    static const char *data_region(const void *shm) {
        return static_cast<const char *>(shm) + sizeof(RingHeader);
    }
    static constexpr std::size_t mask(uint64_t pos) {
        return static_cast<std::size_t>(pos & (Capacity - 1));
    }
};

/// Convenience alias for the default configuration (8MB ring, ~4MB max message).
using DefaultRingBuf = RingBuf<>;

} // namespace shm_ipc

#endif // SHM_IPC_RINGBUF_HPP
