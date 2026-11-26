#pragma once
#include <comm/codec.h>
#include <array>
#include <cstring>
#include <cstdint>
#include <shm_ipc/error.hpp>
#ifdef SHM_CHAIN_PROTOBUF
#include "chain_bench.pb.h"
#endif

namespace chain_bench {
constexpr uint32_t kMagic     = 0x53434210;
constexpr uint32_t kHeader    = 16;
constexpr uint32_t kProtoFlag = 4;
#ifdef SHM_CHAIN_PROTOBUF
constexpr uint32_t kCodecFlag = kProtoFlag;
#else
constexpr uint32_t kCodecFlag = 0;
#endif
constexpr uint32_t kMaxBody    = 1024 * 1024;
constexpr uint32_t kQueueBytes = 8 * 1024 * 1024;
struct Frame
{
    const unsigned char* data = nullptr;
    uint32_t bytes            = 0;
#ifdef SHM_CHAIN_PROTOBUF
    const chain_bench_pb::Rpc* message = nullptr;
#endif
};
inline uint32_t Read32(const unsigned char* p) noexcept
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void Write32(unsigned char* p, uint32_t v) noexcept
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
inline uint64_t Sequence(const Frame& f) noexcept { return (uint64_t(Read32(f.data + 8)) << 32) | Read32(f.data + 12); }
inline void WriteSequence(unsigned char* p, uint64_t n) noexcept
{
    Write32(p + 8, n >> 32);
    Write32(p + 12, n);
}
// Complete=positive frame bytes, incomplete=0, protocol error=-1.
inline int32_t Probe(const unsigned char* p, size_t n) noexcept
{
    if (n < kHeader)
        return 0;
    const auto body = Read32(p + 4);
    if ((Read32(p) & ~kProtoFlag) < kMagic || (Read32(p) & ~kProtoFlag) > kMagic + 2 || body < 16 || body > kMaxBody)
        return -1;
    return n < body + kHeader ? 0 : static_cast<int32_t>(body + kHeader);
}
#ifdef SHM_CHAIN_PROTOBUF
inline uint32_t ProtoPayloadBytes(uint32_t serialized) noexcept
{
    for (uint32_t width = 1; width <= 3; ++width)
    {
        if (serialized <= 10 + width)
            continue;
        const auto payload    = serialized - 10 - width;
        const uint32_t actual = payload < 128 ? 1 : payload < 16384 ? 2 : 3;
        if (actual == width)
            return payload;
    }
    return 0;
}
#endif
inline bool Verify(const Frame& f, uint32_t body, bool full) noexcept
{
    if (f.bytes != body + kHeader)
        return false;
#ifdef SHM_CHAIN_PROTOBUF
    if (!f.message || !f.message->has_sequence() || f.message->sequence() != Sequence(f) ||
        f.message->payload().size() != ProtoPayloadBytes(body))
        return false;
    const auto length = static_cast<uint32_t>(f.message->payload().size());
    const auto* p     = reinterpret_cast<const unsigned char*>(f.message->payload().data());
#else
    const auto length = body;
    const auto* p     = f.data + kHeader;
#endif
    if (full)
    {
        for (uint32_t i = 0; i < length; ++i)
            if (p[i] != static_cast<unsigned char>(i * 131u + 17u))
                return false;
        return true;
    }
    return p[0] == 17 && p[length - 1] == static_cast<unsigned char>((length - 1) * 131u + 17u);
}
// Borrowed view valid only during the callback. The std::any contains a pointer
// (libstdc++ inline storage), not a per-message owning payload allocation.
class WireCodec final : public comm::codec::Codec
{
    Frame view_;

 public:
    static const Frame* Get(const std::any& p) noexcept
    {
        auto slot = std::any_cast<const Frame*>(&p);
        return slot ? *slot : nullptr;
    }
    int32_t EncodedSize(const std::any& p) const override
    {
        const auto* f = Get(p);
        return f && f->data && f->bytes >= kHeader && f->bytes <= kHeader + kMaxBody ? int32_t(f->bytes) : -1;
    }
    int32_t Encode(const std::any& p, comm::byte* dst, size_t available, size_t* need) override
    {
        const auto* f = Get(p);
        const auto n  = EncodedSize(p);
        if (n < 0)
            return -1;
        if (need)
            *need = size_t(n);
        if (available < size_t(n))
            return 0;
        std::memcpy(dst, f->data, size_t(n));
        return n;
    }
    int32_t Decode(const comm::byte* p, size_t n, std::any& out, size_t* need) override
    {
        const auto* raw = reinterpret_cast<const unsigned char*>(p);
        const auto used = Probe(raw, n);
        if (need)
            *need = n < kHeader ? kHeader : Read32(raw + 4) + kHeader;
        if (used <= 0)
            return used;
        view_     = {raw, uint32_t(used)};
        auto slot = std::any_cast<const Frame*>(&out);
        if (slot)
            *slot = &view_;
        else
            out = static_cast<const Frame*>(&view_);
        return used;
    }
};
using PacketHandler  = shm::ErrorCode (*)(void*, const Frame&);
using CloseHandler   = void (*)(void*, shm::ErrorCode);
using ConnectHandler = void (*)(void*);
}  // namespace chain_bench
