/**
 * @file codec.hpp
 * @brief POD 编解码器（C++17，纯头文件）
 *
 * 两层能力：
 * 1. 底层纯函数 Encode / Decode —— 与传输无关，整帧编解码
 * 2. 上层 RingChannel 接口
 *    - SendPod：编码并写入 channel
 *    - PodReader<T>：零拷贝流式读取器，直接读 ring 的 read_region，
 *      支持对端将一个 POD 拆成 N 条 ring 消息发送
 *
 * Codec 帧格式：
 * @code
 * ┌──────────────┬──────────────────────┐
 * │ type_tag u32 │ payload (sizeof(T))  │
 * └──────────────┴──────────────────────┘
 * @endcode
 *
 * PodReader 性能路径：
 *   - 快路径：ring 消息 >= 帧大小且无残留 → PeekRead 零拷贝，1 次 memcpy 到 out
 *   - 慢路径：分片到达 → 仅拷贝所需字节到定长 spill buffer，无 vector/erase
 */

#ifndef SHM_IPC_CODEC_HPP_
#define SHM_IPC_CODEC_HPP_

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "ring_channel.hpp"

namespace shm_ipc {

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

/// @brief 编码帧中 type_tag 占用的字节数
inline constexpr uint32_t kTagSize = sizeof(uint32_t);

// ---------------------------------------------------------------------------
// 底层：一次性 Encode / Decode
// ---------------------------------------------------------------------------

/**
 * @brief 将 POD 对象编码为 [type_tag][payload] 字节帧
 * @tparam T 已注册的 POD 类型
 * @param obj      待编码对象
 * @param buf      输出缓冲区
 * @param buf_size 缓冲区大小（字节）
 * @return 写入的总字节数（kTagSize + sizeof(T)），空间不足返回 0
 */
template <typename T>
uint32_t Encode(const T &obj, void *buf, uint32_t buf_size)
{
    constexpr uint32_t frame_size = kTagSize + sizeof(T);
    if (buf_size < frame_size)
        return 0;

    auto *p                = static_cast<char *>(buf);
    constexpr uint32_t tag = TypeTag<T>::value;
    std::memcpy(p, &tag, kTagSize);
    std::memcpy(p + kTagSize, &obj, sizeof(T));
    return frame_size;
}

/**
 * @brief 从完整字节帧中解码 POD 对象，校验 type_tag 和长度
 * @tparam T 已注册的 POD 类型
 * @param buf 输入字节帧
 * @param len 帧长度（字节）
 * @param out 输出对象指针
 * @return true 解码成功，false 标签不匹配或长度错误
 */
template <typename T>
bool Decode(const void *buf, uint32_t len, T *out)
{
    constexpr uint32_t frame_size = kTagSize + sizeof(T);
    if (len < frame_size)
        return false;

    auto *p      = static_cast<const char *>(buf);
    uint32_t tag = 0;
    std::memcpy(&tag, p, kTagSize);
    if (tag != TypeTag<T>::value)
        return false;

    std::memcpy(out, p + kTagSize, sizeof(T));
    return true;
}

// ---------------------------------------------------------------------------
// 上层：SendPod
// ---------------------------------------------------------------------------

/**
 * @brief 编码 POD 并写入 RingChannel，成功后通知对端
 * @tparam T   已注册的 POD 类型
 * @tparam Cap RingChannel 容量
 * @param ch   双向环形通道
 * @param obj  待发送对象
 * @param seq  消息序列号
 * @return 0 成功，-1 缓冲区满
 */
template <typename T, std::size_t Cap>
int SendPod(RingChannel<Cap> &ch, const T &obj, uint32_t seq)
{
    constexpr uint32_t frame_size = kTagSize + sizeof(T);
    char buf[frame_size];
    Encode(obj, buf, frame_size);

    int rc = ch.TryWrite(buf, frame_size, seq);
    if (rc == 0)
        ch.NotifyPeer();
    return rc;
}

/**
 * @brief 编码 POD 并写入批量写入器（不触发通知，由 Flush 统一通知）
 * @tparam T   已注册的 POD 类型
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
    constexpr uint32_t frame_size = kTagSize + sizeof(T);
    char buf[frame_size];
    Encode(obj, buf, frame_size);
    return batch.TryWrite(buf, frame_size, seq);
}

// ---------------------------------------------------------------------------
// 上层：PodReader —— 零拷贝流式读取器
// ---------------------------------------------------------------------------

/**
 * @brief 零拷贝 POD 读取器，直接操作 RingChannel 的 read_region
 *
 * 通过 PeekRead 获取 ring 内部指针，避免中间缓冲区拷贝。
 * 仅当 codec 帧跨越多条 ring 消息（分片场景）时，才使用
 * 定长 spill buffer 拼接，无 vector、无 erase。
 *
 * @tparam T 已注册的 trivially_copyable POD 类型
 */
template <typename T>
class PodReader
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "PodReader<T>: T must be trivially copyable");

 public:
    static constexpr uint32_t kFrameSize = kTagSize + sizeof(T);

    /**
     * @brief 尝试从 RingChannel 中零拷贝读取一个完整 POD
     *
     * 快路径：spill 无残留且 ring 消息 >= kFrameSize
     *   → 直接从 ring 指针解码，1 次 memcpy(sizeof(T))
     *
     * 慢路径：消息不足一帧或有残留
     *   → 按需拷贝字节到 spill_[]，攒够后解码
     *
     * @tparam Cap RingChannel 容量
     * @param ch   双向环形通道
     * @param out  输出对象指针
     * @return 0 成功，-1 数据不足
     */
    template <std::size_t Cap>
    int TryRecv(RingChannel<Cap> &ch, T *out)
    {
        // spill 中已攒够一帧 → 直接解码
        if (used_ >= kFrameSize)
            return PopSpill(out);

        for (;;)
        {
            const void *data = nullptr;
            uint32_t len     = 0;
            uint32_t seq     = 0;
            if (ch.PeekRead(&data, &len, &seq) != 0)
                return -1;  // ring 空

            auto *src = static_cast<const char *>(data);

            // 快路径：spill 无残留，且本条 ring 消息能覆盖整帧
            if (used_ == 0 && len >= kFrameSize)
            {
                // 直接从 ring 指针解码
                uint32_t tag = 0;
                std::memcpy(&tag, src, kTagSize);
                if (tag != TypeTag<T>::value)
                {
                    // tag 不匹配，跳过整条消息
                    ch.CommitRead(len);
                    continue;
                }
                std::memcpy(out, src + kTagSize, sizeof(T));

                // 本条 ring 消息可能还有剩余字节属于下一帧 → 存入 spill
                uint32_t leftover = len - kFrameSize;
                if (leftover > 0)
                {
                    // 限制存入量不超过 spill 容量，多余字节属于更远的帧，
                    // 会在后续 TryRecv 调用中重新从 ring 读取
                    uint32_t to_spill = (leftover <= kFrameSize) ? leftover : kFrameSize;
                    std::memcpy(spill_, src + kFrameSize, to_spill);
                    used_ = to_spill;
                }

                ch.CommitRead(len);
                return 0;
            }

            // 慢路径：拷贝所需字节到 spill
            uint32_t need = kFrameSize - used_;
            uint32_t take = (len < need) ? len : need;
            std::memcpy(spill_ + used_, src, take);
            used_ += take;

            // 本条 ring 消息被完全或部分消费后都要 commit 整条
            // （ring 是消息粒度的，不支持部分 commit）
            // 如果 ring 消息还有剩余且已攒够帧，剩余存入 spill
            if (used_ >= kFrameSize && take < len)
            {
                uint32_t leftover = len - take;
                // 先把当前 spill 解码腾出空间，再存 leftover
                // 但解码可能失败（tag 不对），此时丢弃该帧
                uint32_t tag = 0;
                std::memcpy(&tag, spill_, kTagSize);
                if (tag != TypeTag<T>::value)
                {
                    used_ = 0;
                    // 把 leftover 作为新的 spill 起点（限制不超过 spill 容量）
                    uint32_t to_spill = (leftover <= kFrameSize) ? leftover : kFrameSize;
                    std::memcpy(spill_, src + take, to_spill);
                    used_ = to_spill;
                    ch.CommitRead(len);
                    continue;
                }
                std::memcpy(out, spill_ + kTagSize, sizeof(T));
                // 存入 leftover（限制不超过 spill 容量）
                {
                    uint32_t to_spill = (leftover <= kFrameSize) ? leftover : kFrameSize;
                    std::memcpy(spill_, src + take, to_spill);
                    used_ = to_spill;
                }
                ch.CommitRead(len);
                return 0;
            }

            ch.CommitRead(len);

            // spill 攒够了
            if (used_ >= kFrameSize)
                return PopSpill(out);

            // 否则继续读下一条 ring 消息
        }
    }

    /** @brief spill buffer 中残留字节数 */
    uint32_t Buffered() const noexcept { return used_; }

    /** @brief 重置内部状态 */
    void Reset() noexcept { used_ = 0; }

 private:
    int PopSpill(T *out)
    {
        uint32_t tag = 0;
        std::memcpy(&tag, spill_, kTagSize);
        if (tag != TypeTag<T>::value)
        {
            used_ = 0;
            return -1;
        }
        std::memcpy(out, spill_ + kTagSize, sizeof(T));
        // 将 spill 中帧后的残留字节前移
        uint32_t remain = used_ - kFrameSize;
        if (remain > 0)
            std::memmove(spill_, spill_ + kFrameSize, remain);
        used_ = remain;
        return 0;
    }

    char spill_[kFrameSize]{};  ///< 分片拼接缓冲区（定长，栈上）
    uint32_t used_ = 0;         ///< spill 中已有字节数
};

}  // namespace shm_ipc

#endif  // SHM_IPC_CODEC_HPP_
