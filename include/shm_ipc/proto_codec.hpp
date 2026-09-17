/**
 * @file proto_codec.hpp
 * @brief Protobuf 类型编解码器，实现 ICodec 接口
 *
 * 提供 ProtoCodec<T> 类。
 * payload 内部格式：[type_name_len u16][type_name chars][pb_payload NB]
 * T 需满足 protobuf Message 接口（GetTypeName, ByteSizeLong, SerializeToArray, ParseFromArray）。
 */

#ifndef SHM_IPC_PROTO_CODEC_HPP_
#define SHM_IPC_PROTO_CODEC_HPP_

#include <cstring>
#include <string>
#include <vector>

#include "codec.hpp"
#include "codec_interface.hpp"
#include "frame_reader.hpp"

namespace shm {

/// @brief type_name 长度前缀占用的字节数（uint16_t）
inline constexpr uint32_t kTypeNameLenSize = sizeof(uint16_t);

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
    // -----------------------------------------------------------------------
    // ICodec 虚方法
    // -----------------------------------------------------------------------

    uint32_t Encode(const void *msg, void *buf,
                    uint32_t buf_size, uint32_t seq) override
    {
        return EncodeTo(*static_cast<const T *>(msg), buf, buf_size, seq);
    }

    bool Decode(const void *buf, uint32_t buf_len,
                const void **payload, uint32_t *payload_len,
                uint32_t *seq) override
    {
        const char *type_name = nullptr;
        uint32_t type_name_len = 0;
        if (!DecodeHeader(buf, buf_len,
                          &type_name, &type_name_len,
                          payload, payload_len, seq))
            return false;
        // 校验 type_name，防止类型混淆
        return TypeNameMatches(type_name, type_name_len);
    }

    std::string TypeName() const override
    {
        T default_instance{};
        return default_instance.GetTypeName();
    }

    /**
     * @brief 从 raw payload 中校验并跳过 type_name 前缀，返回 pb 数据部分
     *
     * type_name 与 T 的实际类型名不匹配时返回 false（防止类型混淆）。
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
        if (!TypeNameMatches(p + kTypeNameLenSize, name_len))
            return false;
        *data     = p + prefix_len;
        *data_len = payload_len - prefix_len;
        return true;
    }

    /// @brief ICodec 虚方法：从 DefaultRingChannel 读取一帧
    int Recv(DefaultRingChannel &ch, void *out) override
    {
        return Recv(ch, static_cast<T *>(out));
    }

    /// @brief ICodec 虚方法：编码 protobuf 并写入 DefaultRingChannel
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
     * @brief 将 protobuf Message 编码为帧到缓冲区
     *
     * 帧格式：[MsgHeader 8B][type_name_len u16][type_name chars][payload NB]
     */
    static uint32_t EncodeTo(const T &msg, void *buf,
                             uint32_t buf_size, uint32_t seq)
    {
        std::string name = msg.GetTypeName();
        auto name_len = static_cast<uint16_t>(name.size());
        auto pb_len = static_cast<uint32_t>(msg.ByteSizeLong());

        uint32_t payload_len = kTypeNameLenSize + name_len + pb_len;
        char *p = EncodeFrame(buf, buf_size, payload_len, seq);
        if (!p)
            return 0;

        std::memcpy(p, &name_len, kTypeNameLenSize);
        p += kTypeNameLenSize;

        std::memcpy(p, name.data(), name_len);
        p += name_len;

        msg.SerializeToArray(p, static_cast<int>(pb_len));
        return kMsgHeaderSize + payload_len;
    }

    /**
     * @brief 从完整字节帧中解析 protobuf 帧头，提取 type_name 和 payload 指针
     */
    static bool DecodeHeader(const void *buf, uint32_t buf_len,
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
        // 减法形式避免 hdr.len 接近 UINT32_MAX 时加法溢出回绕
        if (hdr.len > buf_len - kMsgHeaderSize)
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
     */
    static bool DecodeFrom(const void *payload, uint32_t payload_len, T *out)
    {
        return out->ParseFromArray(payload, static_cast<int>(payload_len));
    }

    // -----------------------------------------------------------------------
    // 模板 Send / Recv（任意 Cap）
    // -----------------------------------------------------------------------

    /**
     * @brief 编码 protobuf Message 并写入 RingChannel，成功后通知对端
     *
     * pb 序列化数据通过单次写入完成，pb_len 不得超过
     * RingBuf::max_write_size（Capacity/2），否则返回 -1 且不写入任何数据。
     */
    template <std::size_t Cap>
    static int Send(RingChannel<Cap> &ch, const T &msg, uint32_t seq)
    {
        std::string name = msg.GetTypeName();
        auto name_len = static_cast<uint16_t>(name.size());
        auto pb_len = static_cast<uint32_t>(msg.ByteSizeLong());
        if (pb_len > RingChannel<Cap>::max_write_size)
            return -1;
        uint32_t payload_len = kTypeNameLenSize + name_len + pb_len;

        return SendFrame(ch, payload_len, seq, [&](auto &batch) {
            batch.TryWrite(&name_len, kTypeNameLenSize);
            batch.TryWrite(name.data(), name_len);
            if (pb_len > 0)
                SerializeToBatch(batch, msg, pb_len);
        });
    }

    /**
     * @brief 编码 protobuf Message 并写入批量写入器（不触发通知）
     *
     * pb_len 限制同上单次 Send。
     */
    template <std::size_t Cap>
    static int SendBatch(typename RingChannel<Cap>::ChannelBatchWriter &batch,
                         const T &msg, uint32_t seq)
    {
        std::string name = msg.GetTypeName();
        auto name_len = static_cast<uint16_t>(name.size());
        auto pb_len = static_cast<uint32_t>(msg.ByteSizeLong());
        if (pb_len > RingChannel<Cap>::max_write_size)
            return -1;
        uint32_t payload_len = kTypeNameLenSize + name_len + pb_len;

        return SendFrameBatch<Cap>(batch, payload_len, seq, [&](auto &b) {
            b.TryWrite(&name_len, kTypeNameLenSize);
            b.TryWrite(name.data(), name_len);
            if (pb_len > 0)
                SerializeToBatch(b, msg, pb_len);
        });
    }

    /**
     * @brief 从内部 FrameReader 读取一帧并反序列化为 T
     *
     * type_name 不匹配或解析失败的毒帧与 -2 超大帧会被立即提交丢弃
     * （推进 read_pos 释放环空间）并自动跳过，直到取到一条合法帧或
     * 环空。每次丢弃都推进 read_pos，循环必然终止。
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
                !out->ParseFromArray(data, static_cast<int>(data_len)))
            {
                reader_.Commit(ch);  // 毒帧立即丢弃，避免驻留环中占用空间
                continue;
            }
            return 0;
        }
    }

 private:
    /**
     * @brief 校验帧内 type_name 是否与 T 的实际类型名一致
     */
    static bool TypeNameMatches(const char *type_name, uint32_t type_name_len)
    {
        static const std::string kExpected = T{}.GetTypeName();
        return type_name_len == kExpected.size() &&
               std::memcmp(type_name, kExpected.data(), type_name_len) == 0;
    }

    FrameReader<> reader_;

    /**
     * @brief 将 protobuf 直接序列化到 batch 写入区域
     *
     * 优先使用 Reserve 获取直写指针（零拷贝），跨环尾时回退到临时缓冲区。
     */
    template <typename BatchWriter>
    static void SerializeToBatch(BatchWriter &batch, const T &msg, uint32_t pb_len)
    {
        char *ptr = batch.Reserve(pb_len);
        if (ptr)
        {
            msg.SerializeToArray(ptr, static_cast<int>(pb_len));
            batch.CommitReserve(pb_len);
        }
        else
        {
            // 跨环尾，回退到临时缓冲区
            uint8_t stack_buf[4096];
            std::vector<uint8_t> heap_buf;
            uint8_t *buf = stack_buf;
            if (pb_len > sizeof(stack_buf))
            {
                heap_buf.resize(pb_len);
                buf = heap_buf.data();
            }
            msg.SerializeToArray(buf, static_cast<int>(pb_len));
            batch.TryWrite(buf, pb_len);
        }
    }
};

}  // namespace shm

#endif  // SHM_IPC_PROTO_CODEC_HPP_
