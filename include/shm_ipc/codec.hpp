/**
 * @file codec.hpp
 * @brief 通用消息编解码器（C++17，纯头文件）
 *
 * 两层能力：
 * 1. 底层通用函数 Encode / Decode —— 与传输无关，支持任意 payload（POD、protobuf 等）
 * 2. 上层 RingChannel 接口
 *    - Send：编码并写入 channel
 *    - FrameReader：从字节流中读取完整帧（有状态，支持继承扩展）
 * 3. POD 便利层
 *    - EncodePod / DecodePod / SendPod —— 编译期确定 payload 大小的薄封装
 *
 * 帧格式：
 * @code
 * ┌──────────────┬──────────────┬──────────────────────────┐
 * │ MsgHeader 8B │ type_tag u32 │ payload (N bytes)        │
 * └──────────────┴──────────────┴──────────────────────────┘
 * @endcode
 *
 * MsgHeader 包含：
 *   - len u32：MsgHeader 之后的总长度（= kTagSize + payload 字节数）
 *   - seq u32：消息序列号
 */

#ifndef SHM_IPC_CODEC_HPP_
#define SHM_IPC_CODEC_HPP_

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "ring_channel.hpp"

namespace shm_ipc {

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

/// @brief 编码帧中 type_tag 占用的字节数
inline constexpr uint32_t kTagSize = sizeof(uint32_t);

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

}  // namespace shm_ipc

/**
 * @brief 注册 POD 类型及其 4 字节标签
 * @param Type      要注册的结构体类型
 * @param tag_value uint32_t 类型标签值
 */
#define SHM_IPC_REGISTER_POD(Type, tag_value)           \
    static_assert(std::is_trivially_copyable_v<Type>,   \
                  #Type " must be trivially copyable"); \
    namespace shm_ipc {                                 \
    template <>                                         \
    struct TypeTag<Type>                                \
    {                                                   \
        static constexpr uint32_t value = (tag_value);  \
    };                                                  \
    }

namespace shm_ipc {

// ===========================================================================
// 通用编解码（自由函数）
// ===========================================================================

/**
 * @brief 将 payload 编码为 [MsgHeader][type_tag][payload] 字节帧
 *
 * @param tag          消息类型标签（uint32_t）
 * @param payload      序列化后的数据指针
 * @param payload_len  数据字节数
 * @param buf          输出缓冲区
 * @param buf_size     缓冲区大小（字节）
 * @param seq          消息序列号
 * @return 写入的总字节数，空间不足返回 0
 */
inline uint32_t Encode(uint32_t tag, const void *payload, uint32_t payload_len,
                       void *buf, uint32_t buf_size, uint32_t seq)
{
    uint32_t hdr_len    = kTagSize + payload_len;
    uint32_t frame_size = kMsgHeaderSize + hdr_len;
    if (buf_size < frame_size)
        return 0;

    auto *p = static_cast<char *>(buf);

    MsgHeader hdr{};
    hdr.len = hdr_len;
    hdr.seq = seq;
    std::memcpy(p, &hdr, kMsgHeaderSize);
    std::memcpy(p + kMsgHeaderSize, &tag, kTagSize);
    if (payload_len > 0)
        std::memcpy(p + kMsgHeaderSize + kTagSize, payload, payload_len);
    return frame_size;
}

/**
 * @brief 从完整字节帧中解码消息，提取 tag、payload 指针和 seq
 *
 * payload 指针直接指向 buf 内部（零拷贝），调用者不应在 buf 释放后使用。
 *
 * @param buf              输入字节帧
 * @param buf_len          帧长度（字节）
 * @param[out] tag         消息类型标签
 * @param[out] payload     payload 指针（指向 buf 内部）
 * @param[out] payload_len payload 字节数
 * @param[out] seq         序列号（可为 nullptr）
 * @return true 解码成功，false 帧不完整或格式错误
 */
inline bool Decode(const void *buf, uint32_t buf_len,
                   uint32_t *tag, const void **payload, uint32_t *payload_len,
                   uint32_t *seq = nullptr)
{
    if (buf_len < kMsgHeaderSize + kTagSize)
        return false;

    auto *p = static_cast<const char *>(buf);

    MsgHeader hdr{};
    std::memcpy(&hdr, p, kMsgHeaderSize);

    if (hdr.len < kTagSize)
        return false;
    if (kMsgHeaderSize + hdr.len > buf_len)
        return false;

    std::memcpy(tag, p + kMsgHeaderSize, kTagSize);
    *payload     = p + kMsgHeaderSize + kTagSize;
    *payload_len = hdr.len - kTagSize;
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
 * @param tag         消息类型标签
 * @param payload     序列化后的数据指针
 * @param payload_len 数据字节数
 * @param seq         消息序列号
 * @return 0 成功，-1 缓冲区满或消息过大
 */
template <std::size_t Cap>
int Send(RingChannel<Cap> &ch, uint32_t tag,
         const void *payload, uint32_t payload_len, uint32_t seq)
{
    uint32_t frame_size = kMsgHeaderSize + kTagSize + payload_len;
    if (ch.WritableBytes() < frame_size)
        return -1;

    // 用 BatchWriter 分段写入，避免堆分配临时缓冲区
    auto batch = ch.StartBatch();

    // 1) MsgHeader
    MsgHeader hdr{};
    hdr.len = kTagSize + payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);

    // 2) type_tag
    batch.TryWrite(&tag, kTagSize);

    // 3) payload
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
 * @param tag         消息类型标签
 * @param payload     序列化后的数据指针
 * @param payload_len 数据字节数
 * @param seq         消息序列号
 * @return 0 成功，-1 缓冲区满或消息过大
 */
template <std::size_t Cap>
int Send(typename RingChannel<Cap>::ChannelBatchWriter &batch, uint32_t tag,
         const void *payload, uint32_t payload_len, uint32_t seq)
{
    uint32_t frame_size = kMsgHeaderSize + kTagSize + payload_len;
    if (batch.FreeBytes() < frame_size)
        return -1;

    MsgHeader hdr{};
    hdr.len = kTagSize + payload_len;
    hdr.seq = seq;
    batch.TryWrite(&hdr, kMsgHeaderSize);
    batch.TryWrite(&tag, kTagSize);
    if (payload_len > 0)
        batch.TryWrite(payload, payload_len);
    return 0;
}

// ===========================================================================
// 通用帧读取器
// ===========================================================================

/**
 * @brief 零拷贝帧读取器
 *
 * 使用 Peek + CommitRead 直接返回 ring buffer 内部指针，避免内存拷贝。
 * 帧数据跨越环尾回绕时，退化为拷贝到内部缓冲区（少见路径）。
 *
 * 使用模式：TryRecv 成功后，payload 指针在下次 TryRecv 前有效
 * （下次 TryRecv 会推进 read_pos，释放上一帧占用的环空间）。
 *
 * @tparam BufSize 回绕时的回退缓冲区大小，默认 64KB
 */
template <uint32_t BufSize = 64 * 1024>
class FrameReader
{
 public:
    virtual ~FrameReader() = default;

    /**
     * @brief 尝试从 RingChannel 中零拷贝读取一个完整帧
     *
     * payload 指针直接指向 ring buffer 内部（零拷贝快速路径），
     * 仅在帧跨环尾回绕时拷贝到内部缓冲区。
     *
     * @tparam Cap RingChannel 容量
     * @param ch               双向环形通道
     * @param[out] tag         消息类型标签
     * @param[out] payload     payload 指针（指向 ring 内部或内部缓冲区）
     * @param[out] payload_len payload 字节数
     * @return 0 成功，-1 数据不足，-2 帧过大（超过 BufSize）
     */
    template <std::size_t Cap>
    int TryRecv(RingChannel<Cap> &ch, uint32_t *tag,
                const void **payload, uint32_t *payload_len)
    {
        Commit(ch);

        // Peek 获取环中所有可读数据
        const void *seg1 = nullptr;
        uint32_t seg1_len = 0;
        const void *seg2 = nullptr;
        uint32_t seg2_len = 0;

        if (ch.Peek(&seg1, &seg1_len, &seg2, &seg2_len) != 0)
            return -1;

        uint32_t total_avail = seg1_len + seg2_len;

        // 至少需要 MsgHeader
        if (total_avail < kMsgHeaderSize)
            return -1;

        // 解析 MsgHeader（可能跨段）
        MsgHeader hdr{};
        auto *s1 = static_cast<const char *>(seg1);
        if (seg1_len >= kMsgHeaderSize)
        {
            std::memcpy(&hdr, s1, kMsgHeaderSize);
        }
        else
        {
            // header 跨段——极少见
            std::memcpy(&hdr, s1, seg1_len);
            std::memcpy(reinterpret_cast<char *>(&hdr) + seg1_len,
                        seg2, kMsgHeaderSize - seg1_len);
        }

        // 校验帧头
        if (hdr.len < kTagSize)
            return -1;
        if (hdr.len > BufSize)
            return -2;

        uint32_t frame_size = kMsgHeaderSize + hdr.len;
        if (total_avail < frame_size)
            return -1;

        hdr_ = hdr;

        // tag+payload 起始位置在帧头之后
        uint32_t body_offset = kMsgHeaderSize;
        uint32_t body_len = hdr.len;  // = kTagSize + payload

        if (body_offset + body_len <= seg1_len)
        {
            // 快速路径：整帧在 seg1 中，零拷贝
            auto *body = s1 + body_offset;
            std::memcpy(tag, body, kTagSize);
            *payload     = body + kTagSize;
            *payload_len = body_len - kTagSize;
        }
        else
        {
            // 慢速路径：body 跨段，拷贝到 buf_
            CopyFromSegments(s1, seg1_len,
                             static_cast<const char *>(seg2), seg2_len,
                             body_offset, buf_, body_len);
            std::memcpy(tag, buf_, kTagSize);
            *payload     = buf_ + kTagSize;
            *payload_len = body_len - kTagSize;
        }

        pending_commit_ = frame_size;
        return 0;
    }

    /**
     * @brief 手动提交上一帧的读取，推进 read_pos
     *
     * TryRecv 会自动提交上一帧，通常无需手动调用。
     * 用于 FrameReader 销毁前或不再调用 TryRecv 时，确保 read_pos 推进。
     */
    template <std::size_t Cap>
    void Commit(RingChannel<Cap> &ch)
    {
        if (pending_commit_ > 0)
        {
            ch.CommitRead(pending_commit_);
            pending_commit_ = 0;
        }
    }

    /** @brief 最近成功读取的消息序列号 */
    uint32_t LastSeq() const { return hdr_.seq; }

 protected:
    MsgHeader hdr_{};
    uint32_t pending_commit_ = 0;  ///< 上一帧的总长度，下次 TryRecv 时提交
    char buf_[BufSize]{};          ///< 回绕时的回退缓冲区

 private:
    /**
     * @brief 从 Peek 返回的两段数据中拷贝指定偏移和长度的字节
     */
    static void CopyFromSegments(const char *s1, uint32_t s1_len,
                                 const char *s2, uint32_t /*s2_len*/,
                                 uint32_t offset, char *dst, uint32_t len)
    {
        uint32_t copied = 0;
        // seg1 中可用的部分
        if (offset < s1_len)
        {
            uint32_t avail = s1_len - offset;
            uint32_t n = (avail < len) ? avail : len;
            std::memcpy(dst, s1 + offset, n);
            copied += n;
        }
        // 剩余从 seg2 拷贝
        if (copied < len)
        {
            uint32_t s2_offset = (offset > s1_len) ? (offset - s1_len) : 0;
            std::memcpy(dst + copied, s2 + s2_offset, len - copied);
        }
    }
};

// ===========================================================================
// POD 便利层
// ===========================================================================

/**
 * @brief 将 POD 对象编码为帧，tag 由 TypeTag<T> 自动提供
 *
 * @tparam T 已通过 SHM_IPC_REGISTER_POD 注册的 trivially_copyable 类型
 * @param obj      待编码对象
 * @param buf      输出缓冲区
 * @param buf_size 缓冲区大小（字节）
 * @param seq      消息序列号
 * @return 写入的总字节数，空间不足返回 0
 */
template <typename T>
uint32_t EncodePod(const T &obj, void *buf, uint32_t buf_size, uint32_t seq)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "EncodePod<T>: T must be trivially copyable");
    return Encode(TypeTag<T>::value, &obj, sizeof(T), buf, buf_size, seq);
}

/**
 * @brief 从 payload 中解码 POD 对象（校验大小）
 *
 * @tparam T 已注册的 trivially_copyable 类型
 * @param payload     payload 指针（由 Decode 或 FrameReader::TryRecv 返回）
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
 * @brief 编码 POD 并写入 RingChannel，成功后通知对端
 *
 * @tparam T   已注册的 trivially_copyable 类型
 * @tparam Cap RingChannel 容量
 * @param ch   双向环形通道
 * @param obj  待发送对象
 * @param seq  消息序列号
 * @return 0 成功，-1 缓冲区满
 */
template <typename T, std::size_t Cap>
int SendPod(RingChannel<Cap> &ch, const T &obj, uint32_t seq)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "SendPod<T>: T must be trivially copyable");
    return Send(ch, TypeTag<T>::value, &obj, sizeof(T), seq);
}

/**
 * @brief 编码 POD 并写入批量写入器（不触发通知，由 Flush 统一通知）
 *
 * @tparam T   已注册的 trivially_copyable 类型
 * @tparam Cap RingChannel 容量
 * @param batch 通道批量写入器
 * @param obj   待发送对象
 * @param seq   消息序列号
 * @return 0 成功，-1 缓冲区满
 */
template <typename T, std::size_t Cap>
int SendPod(typename RingChannel<Cap>::ChannelBatchWriter &batch,
            const T &obj, uint32_t seq)
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "SendPod<T>: T must be trivially copyable");
    return Send<Cap>(batch, TypeTag<T>::value, &obj, sizeof(T), seq);
}

}  // namespace shm_ipc

#endif  // SHM_IPC_CODEC_HPP_
