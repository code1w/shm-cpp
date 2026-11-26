#pragma once
#include "chain_wire.hpp"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <memory>

namespace chain_bench {
struct ProtoOutgoing {
    const Frame *raw = nullptr;
    const chain_bench_pb::Rpc *message = nullptr;
    uint32_t protobuf_bytes = 0, magic = 0;
    uint32_t Bytes() const noexcept { return raw ? raw->bytes : kHeader + protobuf_bytes; }
};
struct ProtoStats {
    uint64_t serialize_shared = 0, serialize_buffered = 0, raw_shared = 0, raw_buffered = 0;
    uint64_t parses = 0, payload_address_changes = 0;
};
// Endpoint parsing owns a reusable protobuf; sidecars forward serialized frames.
class ProtobufCodec final : public comm::codec::Codec {
    struct Range {
        uintptr_t begin = 0, end = 0;
    };
    std::array<Range, 8> ranges_{};
    size_t range_count_ = 0;
    bool parse_ = false;
    Frame view_;
    chain_bench_pb::Rpc parsed_;
    const char *payload_address_ = nullptr;
    ProtoStats stats_;
    bool SharedTarget(const void *dst, size_t bytes) const noexcept {
        const auto address = reinterpret_cast<uintptr_t>(dst);
        for (size_t i = 0; i < range_count_; ++i)
            if (address >= ranges_[i].begin && address < ranges_[i].end && bytes <= ranges_[i].end - address)
                return true;
        return false;
    }

  public:
    void Configure(bool endpoint) {
        parse_ = endpoint;
        if (endpoint) {
            parsed_.mutable_payload()->reserve(kMaxBody);
            payload_address_ = parsed_.payload().data();
        }
    }
    // Startup-only capture: no procfs lookup or allocation in Encode.
    shm::ErrorCode CaptureSharedMaps() noexcept {
        FILE *file = std::fopen("/proc/self/maps", "r");
        if (!file)
            return shm::ErrorCode::kSystem;
        range_count_ = 0;
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            if (!std::strstr(line, "/memfd:"))
                continue;
            unsigned long begin = 0, end = 0;
            char permissions[5]{};
            if (std::sscanf(line, "%lx-%lx %4s", &begin, &end, permissions) != 3 || permissions[1] != 'w' ||
                permissions[3] != 's')
                continue;
            if (range_count_ == ranges_.size()) {
                std::fclose(file);
                return shm::ErrorCode::kProtocol;
            }
            ranges_[range_count_++] = {uintptr_t(begin), uintptr_t(end)};
        }
        std::fclose(file);
        if (!range_count_)
            return shm::ErrorCode::kProtocol;
        std::sort(ranges_.begin(), ranges_.begin() + range_count_,
                  [](const Range &a, const Range &b) { return a.begin < b.begin; });
        size_t count = 0;
        for (size_t i = 0; i < range_count_; ++i) {
            if (count && ranges_[i].begin == ranges_[count - 1].end)
                ranges_[count - 1].end = ranges_[i].end;
            else
                ranges_[count++] = ranges_[i];
        }
        range_count_ = count;
        return shm::ErrorCode::kOk;
    }
    static const Frame *Get(const std::any &value) noexcept {
        auto p = std::any_cast<const Frame *>(&value);
        return p ? *p : nullptr;
    }
    static const ProtoOutgoing *Out(const std::any &value) noexcept {
        auto p = std::any_cast<const ProtoOutgoing *>(&value);
        return p ? *p : nullptr;
    }
    shm::ErrorCode Prepare(const chain_bench_pb::Rpc &message, uint32_t magic, ProtoOutgoing *out) noexcept {
        if (!message.has_sequence())
            return shm::ErrorCode::kProtocol;
        const auto bytes = message.ByteSizeLong();
        if (bytes < 16 || bytes > kMaxBody)
            return shm::ErrorCode::kInvalidArgument;
        // ByteSizeLong is called exactly once, immediately before cached serialization.
        *out = {nullptr, &message, uint32_t(bytes), magic};
        return shm::ErrorCode::kOk;
    }
    int32_t EncodedSize(const std::any &value) const override {
        auto p = Out(value);
        return p && p->Bytes() >= kHeader + 16 && p->Bytes() <= kHeader + kMaxBody ? int32_t(p->Bytes()) : -1;
    }
    shm::ErrorCode EncodePrepared(const ProtoOutgoing &message, unsigned char *dst, uint32_t available) {
        const auto n = message.Bytes();
        if (n > available)
            return shm::ErrorCode::kInvalidArgument;
        const bool shared = SharedTarget(dst, n);
        if (message.raw) {
            std::memcpy(dst, message.raw->data, n);
            if (shared)
                ++stats_.raw_shared;
            else
                ++stats_.raw_buffered;
        } else {
            Write32(dst, message.magic);
            Write32(dst + 4, message.protobuf_bytes);
            WriteSequence(dst, message.message->sequence());
            const auto *end = message.message->SerializeWithCachedSizesToArray(dst + kHeader);
            if (end != dst + n)
                return shm::ErrorCode::kProtocol;
            if (shared)
                ++stats_.serialize_shared;
            else
                ++stats_.serialize_buffered;
        }
        return shm::ErrorCode::kOk;
    }
    int32_t Encode(const std::any &value, comm::byte *dst, size_t available, size_t *need) override {
        const auto *p = Out(value);
        const auto n = EncodedSize(value);
        if (n < 0)
            return -1;
        if (need)
            *need = size_t(n);
        if (available < size_t(n))
            return 0;
        return EncodePrepared(*p, reinterpret_cast<unsigned char *>(dst), uint32_t(n)) == shm::ErrorCode::kOk ? n : -1;
    }
    shm::ErrorCode DecodeFrame(const Frame &frame, std::any *out) {
        if (!(Read32(frame.data) & kProtoFlag))
            return shm::ErrorCode::kProtocol;
        view_ = frame;
        view_.message = nullptr;
        if (parse_) {
            if (!parsed_.ParseFromArray(frame.data + kHeader, int(frame.bytes - kHeader)) || !parsed_.has_sequence() ||
                parsed_.sequence() != Sequence(frame) ||
                parsed_.payload().size() != ProtoPayloadBytes(frame.bytes - kHeader))
                return shm::ErrorCode::kProtocol;
            ++stats_.parses;
            const char *address = parsed_.payload().data();
            if (address != payload_address_)
                ++stats_.payload_address_changes;
            payload_address_ = address;
            view_.message = &parsed_;
        }
        auto p = std::any_cast<const Frame *>(out);
        if (p)
            *p = &view_;
        else
            *out = static_cast<const Frame *>(&view_);
        return shm::ErrorCode::kOk;
    }
    int32_t Decode(const comm::byte *bytes, size_t available, std::any &out, size_t *need) override {
        const auto *data = reinterpret_cast<const unsigned char *>(bytes);
        const auto n = Probe(data, available);
        if (need)
            *need = available < kHeader ? kHeader : Read32(data + 4) + kHeader;
        if (n <= 0)
            return n;
        return DecodeFrame(Frame{data, uint32_t(n)}, &out) == shm::ErrorCode::kOk ? n : -1;
    }
    const ProtoStats &Stats() const noexcept { return stats_; }
};
} // namespace chain_bench
