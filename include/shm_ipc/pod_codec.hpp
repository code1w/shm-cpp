/**
 * @file pod_codec.hpp
 * @brief POD 类型编解码器，实现 ICodec 接口
 *
 * 提供 PodCodec<T> 类。
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
    // -----------------------------------------------------------------------
    // ICodec 虚方法
    // -----------------------------------------------------------------------

    /**
     * @brief 将 POD 对象编码为帧
     *
     * @param msg  指向 T 对象的指针
     */
    uint32_t Encode(const void *msg, void *buf,
                    uint32_t buf_size, uint32_t seq) override
    {
        return EncodeTo(*static_cast<const T *>(msg), buf, buf_size, seq);
    }

    /**
     * @brief 从帧中解码，提取 payload 指针（校验类型标签）
     *
     * 返回的 payload 指向 T 的原始字节（跳过 tag），调用方可用 DecodeFrom 或 memcpy 还原。
     * tag 与 TypeTag<T>::value 不匹配时返回 false。
     */
    bool Decode(const void *buf, uint32_t buf_len,
                const void **payload, uint32_t *payload_len,
                uint32_t *seq) override
    {
        const void *raw_payload = nullptr;
        uint32_t raw_len = 0;
        if (!shm::Decode(buf, buf_len, &raw_payload, &raw_len, seq))
            return false;
        uint32_t tag = 0;
        if (!CheckTag(raw_payload, raw_len, &tag))
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
     * @brief 从 raw payload 中校验并跳过 tag 前缀，返回 T 的原始字节
     *
     * tag 与 TypeTag<T>::value 不匹配时返回 false（防止类型混淆）。
     */
    bool DecodePayload(const void *payload, uint32_t payload_len,
                       const void **data, uint32_t *data_len) override
    {
        uint32_t tag = 0;
        if (!CheckTag(payload, payload_len, &tag))
            return false;
        *data     = static_cast<const char *>(payload) + kTagSize;
        *data_len = payload_len - kTagSize;
        return true;
    }

    /// @brief ICodec 虚方法：从 DefaultRingChannel 读取一帧
    int Recv(DefaultRingChannel &ch, void *out) override
    {
        return Recv(ch, static_cast<T *>(out));
    }

    /// @brief ICodec 虚方法：编码 POD 并写入 DefaultRingChannel
    int Send(DefaultRingChannel &ch, const void *msg, uint32_t seq) override
    {
        return Send(ch, *static_cast<const T *>(msg), seq);
    }

    /// @brief ICodec 虚方法：提交上一帧读取
    void Commit(DefaultRingChannel &ch) override
    {
        reader_.Commit(ch);
    }

    // -----------------------------------------------------------------------
    // 静态工具方法
    // -----------------------------------------------------------------------

    /**
     * @brief 将 POD 对象编码为帧到缓冲区
     *
     * 帧格式：[MsgHeader 8B][tag u32][T bytes]
     */
    static uint32_t EncodeTo(const T &obj, void *buf,
                             uint32_t buf_size, uint32_t seq)
    {
        uint32_t tag = TypeTag<T>::value;
        uint32_t payload_len = kTagSize + sizeof(T);
        char *p = EncodeFrame(buf, buf_size, payload_len, seq);
        if (!p)
            return 0;
        std::memcpy(p, &tag, kTagSize);
        std::memcpy(p + kTagSize, &obj, sizeof(T));
        return kMsgHeaderSize + payload_len;
    }

    /**
     * @brief 从 payload 中解码 POD 对象（校验大小）
     *
     * @param payload     payload 指针（tag 之后的数据）
     * @param payload_len payload 字节数
     * @param[out] out    输出对象指针
     * @return true 解码成功，false 大小不匹配
     */
    static bool DecodeFrom(const void *payload, uint32_t payload_len, T *out)
    {
        if (payload_len != sizeof(T))
            return false;
        std::memcpy(out, payload, sizeof(T));
        return true;
    }

    // -----------------------------------------------------------------------
    // 模板 Send / Recv（任意 Cap）
    // -----------------------------------------------------------------------

    /**
     * @brief 编码 POD 并写入 RingChannel，成功后通知对端
     */
    template <std::size_t Cap>
    static int Send(RingChannel<Cap> &ch, const T &obj, uint32_t seq)
    {
        uint32_t tag = TypeTag<T>::value;
        return SendFrame(ch, kTagSize + sizeof(T), seq, [&](auto &batch) {
            batch.TryWrite(&tag, kTagSize);
            batch.TryWrite(&obj, sizeof(T));
        });
    }

    /**
     * @brief 编码 POD 并写入批量写入器（不触发通知，由 Flush 统一通知）
     */
    template <std::size_t Cap>
    static int SendBatch(typename RingChannel<Cap>::ChannelBatchWriter &batch,
                         const T &obj, uint32_t seq)
    {
        uint32_t tag = TypeTag<T>::value;
        return SendFrameBatch<Cap>(batch, kTagSize + sizeof(T), seq, [&](auto &b) {
            b.TryWrite(&tag, kTagSize);
            b.TryWrite(&obj, sizeof(T));
        });
    }

    /**
     * @brief 从内部 FrameReader 读取一帧并反序列化为 T
     *
     * tag/长度不匹配的毒帧与 -2 超大帧会被立即提交丢弃（推进 read_pos
     * 释放环空间）并自动跳过，直到取到一条合法帧或环空。每次丢弃都
     * 推进 read_pos，循环必然终止。
     *
     * @return 0 成功；-1 无数据；-3 帧头长度非法（协议错误，连接应断开）
     */
    template <std::size_t Cap>
    int Recv(RingChannel<Cap> &ch, T *out)
    {
        for (;;)
        {
            const void *payload = nullptr;
            uint32_t payload_len = 0;
            int rc = reader_.TryRecv(ch, &payload, &payload_len);
            if (rc == -1 || rc == -3)
                return rc;
            if (rc == -2)
            {
                reader_.Commit(ch);  // 超大帧已完整到达，立即丢弃释放环空间
                continue;
            }
            const void *data = nullptr;
            uint32_t data_len = 0;
            if (!DecodePayload(payload, payload_len, &data, &data_len) ||
                data_len != sizeof(T))
            {
                reader_.Commit(ch);  // 毒帧立即丢弃，避免驻留环中占用空间
                continue;
            }
            std::memcpy(out, data, sizeof(T));
            return 0;
        }
    }

 private:
    /**
     * @brief 校验 payload 中的类型标签
     * @return true 标签存在且与 T 注册值匹配
     */
    static bool CheckTag(const void *payload, uint32_t payload_len,
                         uint32_t *tag_out)
    {
        if (payload_len < kTagSize)
            return false;
        std::memcpy(tag_out, payload, kTagSize);
        return *tag_out == TypeTag<T>::value;
    }

    FrameReader<> reader_;
};

}  // namespace shm

#endif  // SHM_IPC_POD_CODEC_HPP_
