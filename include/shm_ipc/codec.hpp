/**
 * @file codec.hpp
 * @brief 通用消息编解码器（C++17，纯头文件）
 *
 * 纯字节流 codec：只负责 [MsgHeader 8B][payload NB] 帧的编解码和传输。
 * 类型标识（POD tag、protobuf type_name 等）由上层 codec 自行写入 payload 内部。
 *
 * 功能层次：
 * 1. 底层通用函数 Encode / Decode —— 与传输无关，支持任意 payload
 * 2. RingChannel 接口
 *    - Send：编码并写入 channel
 *    - FrameReader：从字节流中读取完整帧（有状态，支持继承扩展）
 *
 * 帧格式：
 * @code
 * ┌──────────────┬──────────────────────────────────┐
 * │ MsgHeader 8B │ payload (N bytes)                │
 * └──────────────┴──────────────────────────────────┘
 * @endcode
 *
 * MsgHeader 包含：
 *   - len u32：MsgHeader 之后的总长度（= payload 字节数）
 *   - seq u32：消息序列号
 */

#ifndef SHM_IPC_CODEC_HPP_
#define SHM_IPC_CODEC_HPP_

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "ring_channel.hpp"

namespace shm {

// ---------------------------------------------------------------------------
// 消息帧头
// ---------------------------------------------------------------------------

/** @brief 消息帧头，位于 codec 帧最前端 */
struct MsgHeader
{
    uint32_t len;  ///< MsgHeader 之后的总长度（字节）
    uint32_t seq;  ///< 消息序列号
};

static_assert(sizeof(MsgHeader) == 8, "MsgHeader must be 8 bytes");

/// @brief MsgHeader 占用的字节数
inline constexpr uint32_t kMsgHeaderSize = sizeof(MsgHeader);

// ---------------------------------------------------------------------------
// TypeTag 特化框架
// ---------------------------------------------------------------------------

/**
 * @brief 类型标签特化模板，用户通过 SHM_IPC_REGISTER_POD 注册
 *
 * 未注册的类型会触发 static_assert 编译失败。
 */
template <typename T, typename = void>
struct TypeTag
{
    static_assert(
        sizeof(T) == 0,
        "Type not registered. Use SHM_IPC_REGISTER_POD(Type, tag_value).");
};

}  // namespace shm

/**
 * @brief 注册 POD 类型及其 4 字节标签
 * @param Type      要注册的结构体类型
 * @param tag_value uint32_t 类型标签值
 */
#define SHM_IPC_REGISTER_POD(Type, tag_value)           \
    static_assert(std::is_trivially_copyable_v<Type>,   \
                  #Type " must be trivially copyable"); \
    namespace shm {                                 \
    template <>                                         \
    struct TypeTag<Type>                                \
    {                                                   \
        static constexpr uint32_t value = (tag_value);  \
    };                                                  \
    }

namespace shm {

// ===========================================================================
// 通用编解码（自由函数）
// ===========================================================================

/**
 * @brief 将 payload 编码为 [MsgHeader][payload] 字节帧
 *
 * @param payload      序列化后的数据指针
 * @param payload_len  数据字节数
 * @param buf          输出缓冲区
 * @param buf_size     缓冲区大小（字节）
 * @param seq          消息序列号
 * @return 写入的总字节数，空间不足返回 0
 */
inline uint32_t Encode(const void *payload, uint32_t payload_len,
                       void *buf, uint32_t buf_size, uint32_t seq)
{
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (buf_size < frame_size)
        return 0;

    auto *p = static_cast<char *>(buf);

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    std::memcpy(p, &hdr, kMsgHeaderSize);
    if (payload_len > 0)
        std::memcpy(p + kMsgHeaderSize, payload, payload_len);
    return frame_size;
}

/**
 * @brief 从完整字节帧中解码消息，提取 payload 指针和 seq
 *
 * payload 指针直接指向 buf 内部（零拷贝），调用者不应在 buf 释放后使用。
 *
 * @param buf              输入字节帧
 * @param buf_len          帧长度（字节）
 * @param[out] payload     payload 指针（指向 buf 内部）
 * @param[out] payload_len payload 字节数
 * @param[out] seq         序列号（可为 nullptr）
 * @return true 解码成功，false 帧不完整或格式错误
 */
inline bool Decode(const void *buf, uint32_t buf_len,
                   const void **payload, uint32_t *payload_len,
                   uint32_t *seq = nullptr)
{
    if (buf_len < kMsgHeaderSize)
        return false;

    auto *p = static_cast<const char *>(buf);

    MsgHeader hdr{};
    std::memcpy(&hdr, p, kMsgHeaderSize);

    if (kMsgHeaderSize + hdr.len > buf_len)
        return false;

    *payload     = p + kMsgHeaderSize;
    *payload_len = hdr.len;
    if (seq)
        *seq = hdr.seq;
    return true;
}

// ===========================================================================
// 通用发送
// ===========================================================================

/**
 * @brief 编码消息并写入 RingChannel，成功后通知对端
 *
 * @tparam Cap RingChannel 容量
 * @param ch          双向环形通道
 * @param payload     序列化后的数据指针
 * @param payload_len 数据字节数
 * @param seq         消息序列号
 * @return 0 成功，-1 缓冲区满或消息过大
 */
template <std::size_t Cap>
int Send(RingChannel<Cap> &ch,
         const void *payload, uint32_t payload_len, uint32_t seq)
{
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (ch.WritableBytes() < frame_size)
        return -1;

    auto batch = ch.StartBatch();

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);

    if (payload_len > 0)
        batch.TryWrite(payload, payload_len);

    batch.Flush();
    return 0;
}

/**
 * @brief 编码消息并写入批量写入器（不触发通知，由 Flush 统一通知）
 *
 * @tparam Cap RingChannel 容量
 * @param batch       通道批量写入器
 * @param payload     序列化后的数据指针
 * @param payload_len 数据字节数
 * @param seq         消息序列号
 * @return 0 成功，-1 缓冲区满或消息过大
 */
template <std::size_t Cap>
int Send(typename RingChannel<Cap>::ChannelBatchWriter &batch,
         const void *payload, uint32_t payload_len, uint32_t seq)
{
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (batch.FreeBytes() < frame_size)
        return -1;

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);
    if (payload_len > 0)
        batch.TryWrite(payload, payload_len);
    return 0;
}


}  // namespace shm

#endif  // SHM_IPC_CODEC_HPP_
