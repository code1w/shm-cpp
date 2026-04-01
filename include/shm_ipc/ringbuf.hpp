// ringbuf.hpp — Templated lock-free SPSC ring buffer (C++17)
//
// Memory layout:
// ┌──────────────────────────────────────────────┐
// │ RingHeader (control, cache-line padded)       │
// ├──────────────────────────────────────────────┤
// │ RingSlot[0]: [seq][len][payload...]           │
// │ RingSlot[1]: [seq][len][payload...]           │
// │ ...                                          │
// │ RingSlot[N-1]: [seq][len][payload...]         │
// └──────────────────────────────────────────────┘

#ifndef SHM_IPC_RINGBUF_HPP
#define SHM_IPC_RINGBUF_HPP

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace shm_ipc {

struct RingHeader {
    std::atomic<uint64_t> write_idx;
    char _pad1[56];
    std::atomic<uint64_t> read_idx;
    char _pad2[56];
};

template <int SlotDataSize = 256>
struct RingSlot {
    uint32_t seq;
    uint32_t len;
    char     data[SlotDataSize];
};

// ---------------------------------------------------------------------------
// RingBuf<SlotCount, SlotDataSize>
//
// All operations are static and take a raw shm pointer.  This keeps the ring
// buffer a pure POD memory layout that lives entirely in shared memory —
// no vtable, no heap, no pointers that cross address spaces.
// ---------------------------------------------------------------------------

template <int SlotCount = 16, int SlotDataSize = 256>
class RingBuf {
    static_assert((SlotCount & (SlotCount - 1)) == 0,
                  "SlotCount must be a power of 2");
    static_assert(SlotDataSize > 0, "SlotDataSize must be positive");

public:
    using Slot = RingSlot<SlotDataSize>;

    static constexpr int    slot_count     = SlotCount;
    static constexpr int    slot_data_size = SlotDataSize;
    static constexpr size_t shm_size       = sizeof(RingHeader) + SlotCount * sizeof(Slot);

    // ---- init ----

    static void init(void *shm) {
        std::memset(shm, 0, shm_size);
    }

    // ---- write ----

    /// Try to write; returns -1 if the buffer is full (does not overwrite).
    static int try_write(void *shm, const void *data, uint32_t len, uint32_t seq) {
        auto *hdr   = header(shm);
        auto *slots = slot_array(shm);

        uint64_t w = hdr->write_idx.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_idx.load(std::memory_order_acquire);

        if (w - r >= static_cast<uint64_t>(SlotCount))
            return -1;

        auto &slot = slots[w & (SlotCount - 1)];
        if (len > static_cast<uint32_t>(SlotDataSize)) len = SlotDataSize;
        slot.seq = seq;
        slot.len = len;
        std::memcpy(slot.data, data, len);

        hdr->write_idx.store(w + 1, std::memory_order_release);
        return 0;
    }

    /// Force write — drops the oldest entry when the buffer is full.
    static void force_write(void *shm, const void *data, uint32_t len, uint32_t seq) {
        auto *hdr   = header(shm);
        auto *slots = slot_array(shm);

        uint64_t w = hdr->write_idx.load(std::memory_order_relaxed);
        uint64_t r = hdr->read_idx.load(std::memory_order_acquire);

        if (w - r >= static_cast<uint64_t>(SlotCount))
            hdr->read_idx.store(w - SlotCount + 1, std::memory_order_release);

        auto &slot = slots[w & (SlotCount - 1)];
        if (len > static_cast<uint32_t>(SlotDataSize)) len = SlotDataSize;
        slot.seq = seq;
        slot.len = len;
        std::memcpy(slot.data, data, len);

        hdr->write_idx.store(w + 1, std::memory_order_release);
    }

    // ---- read ----

    /// Try to read one message; returns -1 when empty.
    static int try_read(void *shm, void *data, uint32_t *len, uint32_t *seq) {
        auto *hdr   = header(shm);
        auto *slots = slot_array(shm);

        uint64_t r = hdr->read_idx.load(std::memory_order_relaxed);
        uint64_t w = hdr->write_idx.load(std::memory_order_acquire);

        if (r >= w)
            return -1;

        const auto &slot = slots[r & (SlotCount - 1)];
        if (seq) *seq = slot.seq;
        uint32_t l = slot.len;
        if (l > static_cast<uint32_t>(SlotDataSize)) l = SlotDataSize;
        if (len) *len = l;
        if (data) std::memcpy(data, slot.data, l);

        hdr->read_idx.store(r + 1, std::memory_order_release);
        return 0;
    }

    /// Number of messages available for reading.
    static uint64_t available(const void *shm) {
        auto *hdr = header(shm);
        uint64_t w = hdr->write_idx.load(std::memory_order_acquire);
        uint64_t r = hdr->read_idx.load(std::memory_order_acquire);
        return w - r;
    }

private:
    static RingHeader *header(void *shm) {
        return static_cast<RingHeader *>(shm);
    }
    static const RingHeader *header(const void *shm) {
        return static_cast<const RingHeader *>(shm);
    }
    static Slot *slot_array(void *shm) {
        return reinterpret_cast<Slot *>(static_cast<char *>(shm) + sizeof(RingHeader));
    }
    static const Slot *slot_array(const void *shm) {
        return reinterpret_cast<const Slot *>(
            static_cast<const char *>(shm) + sizeof(RingHeader));
    }
};

/// Convenience alias for the default configuration.
using DefaultRingBuf = RingBuf<16, 256>;

} // namespace shm_ipc

#endif // SHM_IPC_RINGBUF_HPP
