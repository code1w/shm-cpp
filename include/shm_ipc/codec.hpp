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

/// @brief 合法 payload 超出 max_write_size 的最大余量
///
/// codec 层单次发送的序列化数据被限制在 max_write_size（Capacity/2）以内，
/// 但 payload 还包含类型前缀：POD tag 4B，protobuf type_name 前缀最大
/// 2 + 65535B。取 128KB 作为余量，足以覆盖所有前缀又不会放过异常帧。
inline constexpr uint32_t kMaxPayloadSlack = 128 * 1024;

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
// 帧层工具函数
// ===========================================================================

/**
 * @brief 在缓冲区写入 MsgHeader，返回 payload 起始指针
 *
 * 调用者在返回指针处填充 payload 数据。
 *
 * @param buf          输出缓冲区
 * @param buf_size     缓冲区大小（字节）
 * @param payload_len  payload 字节数
 * @param seq          消息序列号
 * @return payload 起始指针（buf + kMsgHeaderSize），空间不足返回 nullptr
 */
inline char *EncodeFrame(void *buf, uint32_t buf_size,
                         uint32_t payload_len, uint32_t seq)
{
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (buf_size < frame_size)
        return nullptr;

    auto *p = static_cast<char *>(buf);
    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    std::memcpy(p, &hdr, kMsgHeaderSize);
    return p + kMsgHeaderSize;
}

/**
 * @brief 写入 MsgHeader + 由回调写 payload，自动 Flush + notify
 *
 * @tparam Cap      RingChannel 容量
 * @tparam WriteFn  回调类型，签名 void(ChannelBatchWriter &)
 * @param ch            双向环形通道
 * @param payload_len   payload 字节数
 * @param seq           消息序列号
 * @param write_payload 回调，负责向 batch 写入 payload
 * @return 0 成功，-1 缓冲区满、消息过大或回调写入字节数与 payload_len 不符
 *
 * @note 回调中每次 TryWrite 的长度不得超过 RingBuf::max_write_size。
 *       函数返回前会校验回调实际写入的字节数恰好等于 payload_len；
 *       不匹配时回滚本次写入（未 Flush 的数据对对端不可见），
 *       保证不会发布残缺帧而损坏字节流。
 */
template <std::size_t Cap, typename WriteFn>
int SendFrame(RingChannel<Cap> &ch, uint32_t payload_len, uint32_t seq,
              WriteFn &&write_payload)
{
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (ch.WritableBytes() < frame_size)
        return -1;

    auto batch = ch.StartBatch();

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);

    write_payload(batch);

    // 防御性校验：回调写入的字节数必须与 payload_len 一致，
    // 否则回滚（不 Flush，数据未发布），避免帧流损坏
    if (batch.PendingBytes() != frame_size)
    {
        batch.Cancel();
        return -1;
    }

    batch.Flush();
    return 0;
}

/**
 * @brief 写入 MsgHeader + 由回调写 payload 到批量写入器（不 Flush）
 *
 * @tparam Cap      RingChannel 容量
 * @tparam WriteFn  回调类型，签名 void(ChannelBatchWriter &)
 * @param batch         通道批量写入器
 * @param payload_len   payload 字节数
 * @param seq           消息序列号
 * @param write_payload 回调，负责向 batch 写入 payload
 * @return 0 成功，-1 缓冲区满、消息过大或回调写入字节数与 payload_len 不符
 *
 * @note 与 SendFrame 一样校验写入字节数，不匹配时回滚本帧，
 *       不影响 batch 中此前已写入的其他帧。
 */
template <std::size_t Cap, typename WriteFn>
int SendFrameBatch(typename RingChannel<Cap>::ChannelBatchWriter &batch,
                   uint32_t payload_len, uint32_t seq,
                   WriteFn &&write_payload)
{
    uint32_t frame_size = kMsgHeaderSize + payload_len;
    if (batch.FreeBytes() < frame_size)
        return -1;

    uint64_t before       = batch.PendingBytes();
    int      before_count = batch.Count();

    MsgHeader hdr{};
    hdr.len = payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);

    write_payload(batch);

    // 回滚必须同时恢复字节计数与写入次数计数：一帧由多次
    // TryWrite 组成（header + 分段 payload），只恢复字节数会
    // 导致 Count() 虚高、Flush() 返回值失真
    if (batch.PendingBytes() - before != frame_size)
    {
        batch.RewindTo(before, before_count);
        return -1;
    }
    return 0;
}

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
    char *p = EncodeFrame(buf, buf_size, payload_len, seq);
    if (!p)
        return 0;
    if (payload_len > 0)
        std::memcpy(p, payload, payload_len);
    return kMsgHeaderSize + payload_len;
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

    // 减法形式避免 hdr.len 接近 UINT32_MAX 时 kMsgHeaderSize + hdr.len 溢出回绕
    if (hdr.len > buf_len - kMsgHeaderSize)
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
 * payload 通过单次 TryWrite 写入，payload_len 不得超过
 * RingBuf::max_write_size（Capacity/2），否则返回 -1 且不写入任何数据。
 */
template <std::size_t Cap>
int Send(RingChannel<Cap> &ch,
         const void *payload, uint32_t payload_len, uint32_t seq)
{
    if (payload_len > RingChannel<Cap>::max_write_size)
        return -1;
    return SendFrame(ch, payload_len, seq, [&](auto &batch) {
        if (payload_len > 0)
            batch.TryWrite(payload, payload_len);
    });
}

/**
 * @brief 编码消息并写入批量写入器（不触发通知，由 Flush 统一通知）
 *
 * payload_len 限制同上单次 Send。
 */
template <std::size_t Cap>
int Send(typename RingChannel<Cap>::ChannelBatchWriter &batch,
         const void *payload, uint32_t payload_len, uint32_t seq)
{
    if (payload_len > RingChannel<Cap>::max_write_size)
        return -1;
    return SendFrameBatch<Cap>(batch, payload_len, seq, [&](auto &b) {
        if (payload_len > 0)
            b.TryWrite(payload, payload_len);
    });
}


}  // namespace shm

#endif  // SHM_IPC_CODEC_HPP_
