/**
 * @file codec_interface.hpp
 * @brief 编解码器纯接口定义
 *
 * ICodec 定义了消息编解码的统一抽象接口，
 * 由 PodCodec（POD 类型）和 ProtoCodec（protobuf 类型）分别实现。
 */

#ifndef SHM_IPC_CODEC_INTERFACE_HPP_
#define SHM_IPC_CODEC_INTERFACE_HPP_

#include <cstdint>
#include <memory>
#include <string>

#include "ring_channel.hpp"

namespace shm {

/**
 * @brief 编解码器纯接口
 *
 * 提供类型无关的编码/解码操作。调用方通过 `const void*` 传入/取出消息，
 * 具体类型由实现类（PodCodec / ProtoCodec）负责解释。
 */
class ICodec
{
 public:
    virtual ~ICodec() = default;

    /**
     * @brief 将消息编码为字节帧
     *
     * @param msg       消息指针（POD: 指向 T 对象；Protobuf: 指向 Message 对象）
     * @param buf       输出缓冲区
     * @param buf_size  缓冲区大小（字节）
     * @param seq       消息序列号
     * @return 写入的总字节数，空间不足返回 0
     */
    virtual uint32_t Encode(const void *msg, void *buf,
                            uint32_t buf_size, uint32_t seq) = 0;

    /**
     * @brief 从字节帧中解码，提取 payload 指针（零拷贝）
     *
     * payload 指针指向 buf 内部，调用方不应在 buf 释放后使用。
     *
     * @param buf              输入字节帧
     * @param buf_len          帧长度（字节）
     * @param[out] payload     payload 指针
     * @param[out] payload_len payload 字节数
     * @param[out] seq         序列号（可为 nullptr）
     * @return true 解码成功，false 帧不完整或格式错误
     */
    virtual bool Decode(const void *buf, uint32_t buf_len,
                        const void **payload, uint32_t *payload_len,
                        uint32_t *seq) = 0;

    /**
     * @brief 消息类型标识字符串
     *
     * POD 返回 tag 的十六进制字符串表示，Protobuf 返回 Message::GetTypeName()。
     */
    virtual std::string TypeName() const = 0;

    /**
     * @brief 从 raw payload 中解码类型前缀，返回数据部分
     *
     * FrameReader 提取 MsgHeader 后的 raw payload 传入此方法，
     * 由具体 codec 跳过自身前缀（POD 的 tag、Protobuf 的 type_name）。
     *
     * @param payload      raw payload 指针（MsgHeader 之后的数据）
     * @param payload_len  payload 字节数
     * @param[out] data    数据部分指针（跳过类型前缀后）
     * @param[out] data_len 数据部分字节数
     * @return true 解码成功，false payload 格式错误
     */
    virtual bool DecodePayload(const void *payload, uint32_t payload_len,
                               const void **data, uint32_t *data_len) = 0;

    /**
     * @brief 从 RingChannel 中读取一帧并反序列化到 out
     *
     * @param ch   双向环形通道（DefaultRingChannel）
     * @param out  输出对象指针（由实现类解释具体类型）
     * @return 0 成功，非 0 无数据或错误
     */
    virtual int Recv(DefaultRingChannel &ch, void *out) = 0;

    /**
     * @brief 编码消息并写入 RingChannel，成功后通知对端
     *
     * @param ch   双向环形通道（DefaultRingChannel）
     * @param msg  消息指针（由实现类解释具体类型）
     * @param seq  消息序列号
     * @return 0 成功，-1 空间不足
     */
    virtual int Send(DefaultRingChannel &ch, const void *msg, uint32_t seq) = 0;

    /**
     * @brief 提交上一帧的读取，推进 read_pos
     */
    virtual void Commit(DefaultRingChannel &ch) = 0;
};

}  // namespace shm

#endif  // SHM_IPC_CODEC_INTERFACE_HPP_
