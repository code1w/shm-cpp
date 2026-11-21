#pragma once
#include <shm_ipc/common.hpp>
#include <shm_ipc/ringbuf.hpp>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define RB_CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

template <size_t Capacity>
struct RingFixture {
    using Ring = shm::RingBuf<Capacity>;
    shm::UniqueFd fd;
    shm::MmapRegion region;
    bool mirrored;
    explicit RingFixture(bool mirror, uint64_t origin = 0) : mirrored(mirror) {
        RB_CHECK(shm::CreateMemfd("ring-proof", Ring::kShmSize, &fd) == shm::ErrorCode::kOk);
        Map();
        Ring::Init(Get());
        auto* h = static_cast<shm::RingHeader*>(Get());
        h->read_pos.store(origin, std::memory_order_relaxed);
        h->write_pos.store(origin, std::memory_order_relaxed);
    }
    void Map() {
        auto ec = mirrored ? region.MapMirrored(fd.Get(), Ring::kShmSize,
            shm::kRingDataOffset, Capacity, PROT_READ | PROT_WRITE, MAP_SHARED)
            : region.Map(fd.Get(), Ring::kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED);
        RB_CHECK(ec == shm::ErrorCode::kOk);
    }
    void* Get() { return region.Get(); }
};
