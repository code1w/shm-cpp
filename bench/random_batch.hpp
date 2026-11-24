#pragma once
#include <cstdint>

namespace acceptance_bench {
// One PRNG step per batch, no allocation and no per-message random generation.
// Synthetic uniform batch counts; requests therefore weight larger batches more.
class RandomBatch {
    uint64_t state_;
    uint32_t minimum_, span_, mask_;
    bool power_of_two_;
 public:
    RandomBatch(uint32_t minimum, uint32_t maximum, uint64_t seed) noexcept
        : state_(seed ? seed : 1), minimum_(minimum), span_(maximum - minimum + 1),
          mask_(span_ - 1), power_of_two_((span_ & mask_) == 0) {}
    void Reset(uint64_t seed) noexcept { state_ = seed ? seed : 1; }
    uint32_t Next() noexcept {
        uint64_t x = state_;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        state_ = x;
        x *= UINT64_C(2685821657736338717);
        // General spans have modulo bias at most (span-1)/2^64 per outcome.
        return minimum_ + static_cast<uint32_t>(power_of_two_ ? (x & mask_) : (x % span_));
    }
};
}
