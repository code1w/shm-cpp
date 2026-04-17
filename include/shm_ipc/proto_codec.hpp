/**
 * @file proto_codec.hpp
 * @brief Protobuf 类型编解码器，实现 ICodec 接口
 *
 * 提供 ProtoCodec<T> 类和 EncodeProto/DecodeProtoHeader/DecodeProto/SendProto 便利函数。
 * payload 内部格式：[type_name_len u16][type_name chars][pb_payload NB]
 * T 需满足 protobuf Message 接口（GetTypeName, ByteSizeLong, SerializeToArray, ParseFromArray）。
 */

#ifndef SHM_IPC_PROTO_CODEC_HPP_
#define SHM_IPC_PROTO_CODEC_HPP_

#include <string>

#include "codec.hpp"
#include "codec_interface.hpp"

namespace shm {

/// @brief type_name 长度前缀占用的字节数（uint16_t）
inline constexpr uint32_t kTypeNameLenSize = sizeof(uint16_t);

// ===========================================================================
// Protobuf 便利函数
// ===========================================================================

/**
 * @brief 将 protobuf Message 编码为帧到缓冲区
 *
 * 帧格式：[MsgHeader 8B][type_name_len u16][type_name chars][payload NB]
 *
 * @tparam T protobuf Message 类型
 */
template <typename T>
uint32_t EncodeProto(const T &msg, void *buf,
                     uint32_t buf_size, uint32_t seq)
{
    std::string name = msg.GetTypeName();
    auto name_len = static_cast<uint16_t>(name.size());
    auto pb_len = static_cast<uint32_t>(msg.ByteSizeLong());

    uint32_t payload_len = kTypeNameLenSize + name_len + pb_len;
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (buf_size < frame_size)
        return 0;

    auto *p = static_cast<char *>(buf);

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    std::memcpy(p, &hdr, kMsgHeaderSize);
    p += kMsgHeaderSize;

    std::memcpy(p, &name_len, kTypeNameLenSize);
    p += kTypeNameLenSize;

    std::memcpy(p, name.data(), name_len);
    p += name_len;

    msg.SerializeToArray(p, static_cast<int>(pb_len));
    return frame_size;
}

/**
 * @brief 从完整字节帧中解析 protobuf 帧头，提取 type_name 和 payload 指针
 *
 * @param buf              输入字节帧
 * @param buf_len          帧长度（字节）
 * @param[out] type_name   type_name 字符串指针（指向 buf 内部，非 null 终止）
 * @param[out] type_name_len type_name 字节数
 * @param[out] payload     payload 指针（指向 buf 内部）
 * @param[out] payload_len payload 字节数
 * @param[out] seq         序列号（可为 nullptr）
 * @return true 解码成功，false 帧不完整或格式错误
 */
inline bool DecodeProtoHeader(const void *buf, uint32_t buf_len,
                              const char **type_name, uint32_t *type_name_len,
                              const void **payload, uint32_t *payload_len,
                              uint32_t *seq = nullptr)
{
    if (buf_len < kMsgHeaderSize + kTypeNameLenSize)
        return false;

    auto *p = static_cast<const char *>(buf);

    MsgHeader hdr{};
    std::memcpy(&hdr, p, kMsgHeaderSize);

    if (hdr.len < kTypeNameLenSize)
        return false;
    if (kMsgHeaderSize + hdr.len > buf_len)
        return false;

    uint16_t name_len = 0;
    std::memcpy(&name_len, p + kMsgHeaderSize, kTypeNameLenSize);

    if (hdr.len < kTypeNameLenSize + name_len)
        return false;

    *type_name     = p + kMsgHeaderSize + kTypeNameLenSize;
    *type_name_len = name_len;
    *payload       = p + kMsgHeaderSize + kTypeNameLenSize + name_len;
    *payload_len   = hdr.len - kTypeNameLenSize - name_len;
    if (seq)
        *seq = hdr.seq;
    return true;
}

/**
 * @brief 从 payload 中反序列化 protobuf Message
 *
 * @tparam T protobuf Message 类型
 */
template <typename T>
bool DecodeProto(const void *payload, uint32_t payload_len, T *out)
{
    return out->ParseFromArray(payload, static_cast<int>(payload_len));
}

/**
 * @brief 编码 protobuf Message 并写入 RingChannel，成功后通知对端
 *
 * @tparam T   protobuf Message 类型
 * @tparam Cap RingChannel 容量
 */
template <typename T, std::size_t Cap>
int SendProto(RingChannel<Cap> &ch, const T &msg, uint32_t seq)
{
    std::string name = msg.GetTypeName();
    auto name_len = static_cast<uint16_t>(name.size());
    std::string pb_data = msg.SerializeAsString();
    auto pb_len = static_cast<uint32_t>(pb_data.size());

    uint32_t payload_len = kTypeNameLenSize + name_len + pb_len;
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (ch.WritableBytes() < frame_size)
        return -1;

    auto batch = ch.StartBatch();

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);
    batch.TryWrite(&name_len, kTypeNameLenSize);
    batch.TryWrite(name.data(), name_len);
    if (pb_len > 0)
        batch.TryWrite(pb_data.data(), pb_len);

    batch.Flush();
    return 0;
}

/**
 * @brief 编码 protobuf Message 并写入批量写入器（不触发通知）
 *
 * @tparam T   protobuf Message 类型
 * @tparam Cap RingChannel 容量
 */
template <typename T, std::size_t Cap>
int SendProto(typename RingChannel<Cap>::ChannelBatchWriter &batch,
              const T &msg, uint32_t seq)
{
    std::string name = msg.GetTypeName();
    auto name_len = static_cast<uint16_t>(name.size());
    std::string pb_data = msg.SerializeAsString();
    auto pb_len = static_cast<uint32_t>(pb_data.size());

    uint32_t payload_len = kTypeNameLenSize + name_len + pb_len;
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (batch.FreeBytes() < frame_size)
        return -1;

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);
    batch.TryWrite(&name_len, kTypeNameLenSize);
    batch.TryWrite(name.data(), name_len);
    if (pb_len > 0)
        batch.TryWrite(pb_data.data(), pb_len);
    return 0;
}

// ===========================================================================
// ProtoCodec<T> — ICodec 实现
// ===========================================================================

/**
 * @brief Protobuf 类型的 ICodec 实现
 *
 * payload 内部格式：[type_name_len u16][type_name chars][pb_payload NB]
 *
 * @tparam T protobuf Message 类型
 */
template <typename T>
class ProtoCodec : public ICodec
{
 public:
    uint32_t Encode(const void *msg, void *buf,
                    uint32_t buf_size, uint32_t seq) override
    {
        return EncodeProto(*static_cast<const T *>(msg), buf, buf_size, seq);
    }

    bool Decode(const void *buf, uint32_t buf_len,
                const void **payload, uint32_t *payload_len,
                uint32_t *seq) override
    {
        const char *type_name = nullptr;
        uint32_t type_name_len = 0;
        return DecodeProtoHeader(buf, buf_len,
                                 &type_name, &type_name_len,
                                 payload, payload_len, seq);
    }

    std::string TypeName() const override
    {
        T default_instance{};
        return default_instance.GetTypeName();
    }

    /**
     * @brief 从 raw payload 中跳过 type_name 前缀，返回 pb 数据部分
     */
    bool DecodePayload(const void *payload, uint32_t payload_len,
                       const void **data, uint32_t *data_len) override
    {
        if (payload_len < kTypeNameLenSize)
            return false;
        auto *p = static_cast<const char *>(payload);
        uint16_t name_len = 0;
        std::memcpy(&name_len, p, kTypeNameLenSize);
        uint32_t prefix_len = kTypeNameLenSize + name_len;
        if (payload_len < prefix_len)
            return false;
        *data     = p + prefix_len;
        *data_len = payload_len - prefix_len;
        return true;
    }

    /**
     * @brief 编码 protobuf Message 并写入 RingChannel，成功后通知对端
     *
     * @tparam Cap RingChannel 容量
     */
    template <std::size_t Cap>
    int Send(RingChannel<Cap> &ch, const T &msg, uint32_t seq)
    {
        return SendProto(ch, msg, seq);
    }
};

}  // namespace shm

#endif  // SHM_IPC_PROTO_CODEC_HPP_
