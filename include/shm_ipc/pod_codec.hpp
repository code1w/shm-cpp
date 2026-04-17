/**
 * @file pod_codec.hpp
 * @brief POD 类型编解码器，实现 ICodec 接口
 *
 * 提供 PodCodec<T> 类和 EncodePod/DecodePod/SendPod 便利函数。
 * payload 内部格式：[tag u32][T bytes]
 * T 必须通过 SHM_IPC_REGISTER_POD 注册。
 */

#ifndef SHM_IPC_POD_CODEC_HPP_
#define SHM_IPC_POD_CODEC_HPP_

#include <cstdio>
#include <string>
#include <type_traits>

#include "codec.hpp"
#include "codec_interface.hpp"
#include "frame_reader.hpp"

namespace shm {

/// @brief POD 帧中 type_tag 占用的字节数
inline constexpr uint32_t kTagSize = sizeof(uint32_t);

// ===========================================================================
// PodCodec<T> — ICodec 实现
// ===========================================================================

/**
 * @brief POD 类型的 ICodec 实现
 *
 * payload 内部格式：[type_tag u32][T bytes]
 *
 * @tparam T 已通过 SHM_IPC_REGISTER_POD 注册的 trivially_copyable 类型
 */
template <typename T>
class PodCodec : public ICodec
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "PodCodec<T>: T must be trivially copyable");

 public:
    /**
     * @brief 将 POD 对象编码为帧
     *
     * @param msg  指向 T 对象的指针
     */
    uint32_t Encode(const void *msg, void *buf,
                    uint32_t buf_size, uint32_t seq) override
    {
        // 构造 payload = [tag][T bytes]
        uint32_t tag = TypeTag<T>::value;
        uint32_t payload_len = kTagSize + sizeof(T);

        uint32_t frame_size = kMsgHeaderSize + payload_len;
        if (buf_size < frame_size)
            return 0;

        auto *p = static_cast<char *>(buf);

        MsgHeader hdr{};
        hdr.len = payload_len;
        hdr.seq = seq;
        std::memcpy(p, &hdr, kMsgHeaderSize);
        std::memcpy(p + kMsgHeaderSize, &tag, kTagSize);
        std::memcpy(p + kMsgHeaderSize + kTagSize, msg, sizeof(T));
        return frame_size;
    }

    /**
     * @brief 从帧中解码，提取 payload 指针
     *
     * 返回的 payload 指向 T 的原始字节（跳过 tag），调用方可用 DecodePod 或 memcpy 还原。
     */
    bool Decode(const void *buf, uint32_t buf_len,
                const void **payload, uint32_t *payload_len,
                uint32_t *seq) override
    {
        const void *raw_payload = nullptr;
        uint32_t raw_len = 0;
        if (!shm::Decode(buf, buf_len, &raw_payload, &raw_len, seq))
            return false;
        if (raw_len < kTagSize)
            return false;
        auto *p = static_cast<const char *>(raw_payload);
        *payload     = p + kTagSize;
        *payload_len = raw_len - kTagSize;
        return true;
    }

    /**
     * @brief 返回 type_tag 的十六进制字符串
     */
    std::string TypeName() const override
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", TypeTag<T>::value);
        return buf;
    }

    /**
     * @brief 从 raw payload 中跳过 tag 前缀，返回 T 的原始字节
     */
    bool DecodePayload(const void *payload, uint32_t payload_len,
                       const void **data, uint32_t *data_len) override
    {
        if (payload_len < kTagSize)
            return false;
        *data     = static_cast<const char *>(payload) + kTagSize;
        *data_len = payload_len - kTagSize;
        return true;
    }

    /**
     * @brief 编码 POD 并写入 RingChannel，成功后通知对端
     *
     * @tparam Cap RingChannel 容量
     */
    template <std::size_t Cap>
    int Send(RingChannel<Cap> &ch, const T &obj, uint32_t seq)
    {
        return SendPod(ch, obj, seq);
    }
};

// ===========================================================================
// POD 便利函数
// ===========================================================================

/**
 * @brief 将 POD 对象编码为帧（含 tag），tag 由 TypeTag<T> 自动提供
 *
 * 帧格式：[MsgHeader 8B][tag u32][T bytes]
 *
 * @tparam T 已通过 SHM_IPC_REGISTER_POD 注册的 trivially_copyable 类型
 */
template <typename T>
uint32_t EncodePod(const T &obj, void *buf, uint32_t buf_size, uint32_t seq)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "EncodePod<T>: T must be trivially copyable");
    // 构造 [tag][T] 然后调用 codec Encode
    uint32_t tag = TypeTag<T>::value;
    uint32_t payload_len = kTagSize + sizeof(T);
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (buf_size < frame_size)
        return 0;

    auto *p = static_cast<char *>(buf);
    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    std::memcpy(p, &hdr, kMsgHeaderSize);
    std::memcpy(p + kMsgHeaderSize, &tag, kTagSize);
    std::memcpy(p + kMsgHeaderSize + kTagSize, &obj, sizeof(T));
    return frame_size;
}

/**
 * @brief 从 payload 中解码 POD 对象（校验大小）
 *
 * @tparam T 已注册的 trivially_copyable 类型
 * @param payload     payload 指针（由 Decode 返回，指向 tag 之后的数据）
 * @param payload_len payload 字节数
 * @param[out] out    输出对象指针
 * @return true 解码成功，false 大小不匹配
 */
template <typename T>
bool DecodePod(const void *payload, uint32_t payload_len, T *out)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "DecodePod<T>: T must be trivially copyable");
    if (payload_len != sizeof(T))
        return false;
    std::memcpy(out, payload, sizeof(T));
    return true;
}

/**
 * @brief 从完整字节帧中解码，提取 tag、payload 指针和 seq
 *
 * 帧格式：[MsgHeader 8B][tag u32][payload]
 * payload 指针直接指向 buf 内部（零拷贝）。
 *
 * @param buf              输入字节帧
 * @param buf_len          帧长度（字节）
 * @param[out] tag         消息类型标签
 * @param[out] payload     payload 指针（指向 buf 内部，tag 之后）
 * @param[out] payload_len payload 字节数
 * @param[out] seq         序列号（可为 nullptr）
 * @return true 解码成功，false 帧不完整或格式错误
 */
inline bool DecodePodFrame(const void *buf, uint32_t buf_len,
                           uint32_t *tag, const void **payload,
                           uint32_t *payload_len, uint32_t *seq = nullptr)
{
    const void *raw_payload = nullptr;
    uint32_t raw_len = 0;
    if (!Decode(buf, buf_len, &raw_payload, &raw_len, seq))
        return false;
    if (raw_len < kTagSize)
        return false;
    auto *p = static_cast<const char *>(raw_payload);
    std::memcpy(tag, p, kTagSize);
    *payload     = p + kTagSize;
    *payload_len = raw_len - kTagSize;
    return true;
}

/**
 * @brief 编码 POD 并写入 RingChannel，成功后通知对端
 *
 * @tparam T   已注册的 trivially_copyable 类型
 * @tparam Cap RingChannel 容量
 */
template <typename T, std::size_t Cap>
int SendPod(RingChannel<Cap> &ch, const T &obj, uint32_t seq)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "SendPod<T>: T must be trivially copyable");
    uint32_t tag = TypeTag<T>::value;
    uint32_t payload_len = kTagSize + sizeof(T);
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (ch.WritableBytes() < frame_size)
        return -1;

    auto batch = ch.StartBatch();

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);
    batch.TryWrite(&tag, kTagSize);
    batch.TryWrite(&obj, sizeof(T));

    batch.Flush();
    return 0;
}

/**
 * @brief 编码 POD 并写入批量写入器（不触发通知，由 Flush 统一通知）
 *
 * @tparam T   已注册的 trivially_copyable 类型
 * @tparam Cap RingChannel 容量
 */
template <typename T, std::size_t Cap>
int SendPod(typename RingChannel<Cap>::ChannelBatchWriter &batch,
            const T &obj, uint32_t seq)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "SendPod<T>: T must be trivially copyable");
    uint32_t tag = TypeTag<T>::value;
    uint32_t payload_len = kTagSize + sizeof(T);
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (batch.FreeBytes() < frame_size)
        return -1;

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);
    batch.TryWrite(&tag, kTagSize);
    batch.TryWrite(&obj, sizeof(T));
    return 0;
}

}  // namespace shm

#endif  // SHM_IPC_POD_CODEC_HPP_
