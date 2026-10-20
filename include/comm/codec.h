/**
 * @file comm/codec.h
 * @brief 编解码器接口（shm 与 framework/io 共享）
 *
 * comm::codec::Codec 的虚函数签名、返回值约定、公开字段与框架
 * codec/codec_interface.h 的 codec::Codec 完全一致：
 *
 *   Encode:  >0 写入字节数；0 需要更大缓冲（*size 为所需大小，0 为内部错误）；
 *            -1 断开连接；-2 丢弃该包
 *   Decode:  >0 消费字节数（pkg 已填充）；0 数据不完整；-1 断开连接；
 *            -2 心跳包（*size 为消费字节数）
 *
 * 迁移现有 codec 的两种方式：
 * 1. 零改动：comm::codec::Adapt(std::make_shared<你的codec>(...)) 包装后即可用
 * 2. 换基类：把 codec 类的基类 codec::Codec 改为 comm::codec::Codec
 */

#ifndef COMM_CODEC_H_
#define COMM_CODEC_H_

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "types.h"

namespace comm {
namespace codec {

/**
 * @brief 编解码器基类（与框架 codec::Codec 接口完全一致）
 */
class Codec
{
 public:
    Codec()          = default;
    virtual ~Codec() = default;

    /// @brief 将消息包编码到 buf，返回值语义见文件头注释
    virtual int32_t Encode(const std::any & /*pkg*/, byte * /*buf*/,
                           size_t /*buflen*/, size_t * /*size*/)
    {
        return 0;
    }

    /// @brief 从 buf 解码一个消息包，返回值语义见文件头注释
    virtual int32_t Decode(const byte * /*buf*/, size_t /*buflen*/,
                           std::any & /*pkg*/, size_t * /*size*/)
    {
        return 0;
    }

    /// @brief 生成心跳响应包写入 buf，返回写入字节数
    virtual int32_t SendKeepAlive(byte * /*buf*/, size_t /*buflen*/,
                                  size_t * /*size*/)
    {
        return 0;
    }

    /// @brief 判断 buf 中的数据是否为心跳包
    virtual bool ParseKeepAlive(const byte * /*buf*/, size_t /*buflen*/,
                                size_t * /*size*/)
    {
        return false;
    }

    bool keepalive{false};   ///< 是否开启心跳（与框架同名字段）
    uint64_t conn_id{0};     ///< 所属连接 ID（与框架同名字段）
};

using CodecPtr = std::shared_ptr<Codec>;  ///< 对应 codec::CodecPtr

/// @brief codec 工厂（每条连接一个实例）
using CodecFactoryCallback = std::function<CodecPtr()>;

// ---------------------------------------------------------------------------
// Adapt — 鸭类型适配器：零改动复用现有 codec 类
// ---------------------------------------------------------------------------

namespace detail {

template <typename T, typename = void>
struct HasSendKeepAlive : std::false_type
{
};

template <typename T>
struct HasSendKeepAlive<
    T, std::void_t<decltype(std::declval<T>().SendKeepAlive(
           static_cast<byte *>(nullptr), static_cast<size_t>(0),
           static_cast<size_t *>(nullptr)))>> : std::true_type
{
};

template <typename T, typename = void>
struct HasParseKeepAlive : std::false_type
{
};

template <typename T>
struct HasParseKeepAlive<
    T, std::void_t<decltype(std::declval<T>().ParseKeepAlive(
           static_cast<const byte *>(nullptr), static_cast<size_t>(0),
           static_cast<size_t *>(nullptr)))>> : std::true_type
{
};

/**
 * @brief 把任意具有 Encode/Decode 同签名方法的 codec 包装为 comm::codec::Codec
 *
 * @tparam T 现有 codec 类型（如框架 codec::Codec 的派生类），无需继承本接口
 */
template <typename T>
class Adapter : public Codec
{
 public:
    explicit Adapter(std::shared_ptr<T> inner) : inner_(std::move(inner)) {}

    int32_t Encode(const std::any &pkg, byte *buf, size_t buflen, size_t *size) override
    {
        return inner_->Encode(pkg, buf, buflen, size);
    }

    int32_t Decode(const byte *buf, size_t buflen, std::any &pkg, size_t *size) override
    {
        return inner_->Decode(buf, buflen, pkg, size);
    }

    int32_t SendKeepAlive(byte *buf, size_t buflen, size_t *size) override
    {
        if constexpr (HasSendKeepAlive<T>::value)
            return inner_->SendKeepAlive(buf, buflen, size);
        else
            return 0;
    }

    bool ParseKeepAlive(const byte *buf, size_t buflen, size_t *size) override
    {
        if constexpr (HasParseKeepAlive<T>::value)
            return inner_->ParseKeepAlive(buf, buflen, size);
        else
            return false;
    }

    /// @brief 访问被包装的原始 codec
    std::shared_ptr<T> Inner() const { return inner_; }

 private:
    std::shared_ptr<T> inner_;
};

}  // namespace detail

/**
 * @brief 包装现有 codec 为 comm::codec::CodecPtr（零改动复用）
 *
 * @code
 * server.SetCodecFactory([] {
 *     return comm::codec::Adapt(std::make_shared<MyFrameworkCodec>());
 * });
 * @endcode
 */
template <typename T>
CodecPtr Adapt(std::shared_ptr<T> inner)
{
    return std::make_shared<detail::Adapter<T>>(std::move(inner));
}

}  // namespace codec
}  // namespace comm

#endif  // COMM_CODEC_H_
