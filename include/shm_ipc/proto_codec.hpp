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
        return DecodeHeader(buf, buf_len,
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
     */
    template <std::size_t Cap>
    static int Send(RingChannel<Cap> &ch, const T &msg, uint32_t seq)
    {
        std::string name = msg.GetTypeName();
        auto name_len = static_cast<uint16_t>(name.size());
        auto pb_len = static_cast<uint32_t>(msg.ByteSizeLong());
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
     */
    template <std::size_t Cap>
    static int SendBatch(typename RingChannel<Cap>::ChannelBatchWriter &batch,
                         const T &msg, uint32_t seq)
    {
        std::string name = msg.GetTypeName();
        auto name_len = static_cast<uint16_t>(name.size());
        auto pb_len = static_cast<uint32_t>(msg.ByteSizeLong());
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
     */
    template <std::size_t Cap>
    int Recv(RingChannel<Cap> &ch, T *out)
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        int rc = reader_.TryRecv(ch, &payload, &payload_len);
        if (rc != 0)
            return rc;
        const void *data = nullptr;
        uint32_t data_len = 0;
        if (!DecodePayload(payload, payload_len, &data, &data_len))
            return -1;
        return out->ParseFromArray(data, static_cast<int>(data_len)) ? 0 : -1;
    }

 private:
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
