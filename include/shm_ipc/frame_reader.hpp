/**
 * @file frame_reader.hpp
 * @brief 零拷贝帧读取器，从 RingChannel 字节流中提取完整 [MsgHeader][payload] 帧
 */

#ifndef SHM_IPC_FRAME_READER_HPP_
#define SHM_IPC_FRAME_READER_HPP_

#include <cstdint>
#include <cstring>

#include "codec.hpp"
#include "codec_interface.hpp"
#include "ring_channel.hpp"

namespace shm {

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
     * @param[out] payload     payload 指针（指向 ring 内部或内部缓冲区）
     * @param[out] payload_len payload 字节数
     * @return 0 成功，-1 数据不足，-2 帧过大（超过 BufSize）
     */
    template <std::size_t Cap>
    int TryRecv(RingChannel<Cap> &ch,
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
        if (hdr.len > BufSize)
            return -2;

        uint32_t frame_size = kMsgHeaderSize + hdr.len;
        if (total_avail < frame_size)
            return -1;

        hdr_ = hdr;

        // payload 起始位置在帧头之后
        uint32_t body_offset = kMsgHeaderSize;
        uint32_t body_len = hdr.len;

        if (body_len == 0)
        {
            *payload     = nullptr;
            *payload_len = 0;
        }
        else if (body_offset + body_len <= seg1_len)
        {
            // 快速路径：整帧在 seg1 中，零拷贝
            *payload     = s1 + body_offset;
            *payload_len = body_len;
        }
        else
        {
            // 慢速路径：body 跨段，拷贝到 buf_
            CopyFromSegments(s1, seg1_len,
                             static_cast<const char *>(seg2), seg2_len,
                             body_offset, buf_, body_len);
            *payload     = buf_;
            *payload_len = body_len;
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
// CodecReader — FrameReader + ICodec 组合读取器
// ===========================================================================

/**
 * @brief 组合 FrameReader 与 ICodec 的读取器
 *
 * FrameReader 提取 raw payload 后，通过 ICodec::DecodePayload 跳过类型前缀，
 * 返回纯数据部分。
 *
 * @tparam BufSize 回绕时的回退缓冲区大小，默认 64KB
 */
template <uint32_t BufSize = 64 * 1024>
class CodecReader : public FrameReader<BufSize>
{
 public:
    explicit CodecReader(ICodec *codec = nullptr) : codec_(codec) {}

    /** @brief 设置/替换关联的编解码器 */
    void SetCodec(ICodec *codec) { codec_ = codec; }

    /**
     * @brief 从 RingChannel 读取一帧并通过 ICodec 解码
     *
     * @tparam Cap RingChannel 容量
     * @param ch               双向环形通道
     * @param[out] data        解码后的数据指针（跳过类型前缀）
     * @param[out] data_len    数据字节数
     * @return 0 成功，-1 数据不足或解码失败，-2 帧过大
     */
    template <std::size_t Cap>
    int TryRecv(RingChannel<Cap> &ch,
                const void **data, uint32_t *data_len)
    {
        const void *payload = nullptr;
        uint32_t payload_len = 0;
        int rc = FrameReader<BufSize>::TryRecv(ch, &payload, &payload_len);
        if (rc != 0)
            return rc;
        if (!codec_ || !codec_->DecodePayload(payload, payload_len, data, data_len))
            return -1;
        return 0;
    }

 private:
    ICodec *codec_;
};

}  // namespace shm

#endif  // SHM_IPC_FRAME_READER_HPP_
