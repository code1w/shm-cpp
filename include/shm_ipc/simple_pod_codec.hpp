/**
 * @file simple_pod_codec.hpp
 * @brief 通用 POD 消息 codec（C++17，std::any 消息模型）
 *
 * 实现 comm::codec::Codec 接口，帧格式：[tag u32][T bytes]，
 * 复用 SHM_IPC_REGISTER_POD 的 TypeTag 注册体系，消息以 std::any 携带值。
 *
 * 用法：
 * @code
 * shm::codec::SimplePodCodec::RegisterPod<Heartbeat>();   // 进程启动时注册一次
 * shm::codec::SimplePodCodec::RegisterPod<ClientMsg>();
 *
 * server.SetCodecFactory([] {
 *     return std::make_shared<shm::codec::SimplePodCodec>();
 * });
 * server.SetMessageCallback([](const comm::TcpConnPtr &conn, const std::any &pkg) {
 *     const auto &msg = std::any_cast<const ClientMsg &>(pkg);
 *     ...
 *     conn->SendCmd(Heartbeat{...});
 * });
 * @endcode
 */

#ifndef SHM_IPC_SIMPLE_POD_CODEC_HPP_
#define SHM_IPC_SIMPLE_POD_CODEC_HPP_

#include <any>
#include <cstring>
#include <functional>
#include <typeindex>
#include <type_traits>
#include <unordered_map>

#include <comm/codec.h>

#include "codec.hpp"
#include "pod_codec.hpp"  // kTagSize

namespace shm {
namespace codec {

/**
 * @brief 通用 POD 消息 codec
 *
 * 帧格式：[tag u32][T bytes]。tag 来自 SHM_IPC_REGISTER_POD 注册值。
 * 编码按 std::any 实际类型查表，解码按 tag 查表构造对应类型。
 */
class SimplePodCodec : public comm::codec::Codec
{
 public:
    /**
     * @brief 注册 POD 类型（进程启动时对每种消息调用一次）
     * @tparam T 已经过 SHM_IPC_REGISTER_POD 注册的 trivially_copyable 类型
     */
    template <typename T>
    static void RegisterPod()
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "SimplePodCodec::RegisterPod<T>: T must be trivially copyable");
        TagOf()[std::type_index(typeid(T))] = TypeTag<T>::value;
        OpsOf()[TypeTag<T>::value]          = TypeOps{
            sizeof(T),
            // encode：从 any 取出 T 的字节
            [](const std::any &pkg, void *out) -> bool {
                const T *p = std::any_cast<T>(&pkg);
                if (!p)
                    return false;
                std::memcpy(out, p, sizeof(T));
                return true;
            },
            // decode：从字节构造 any
            [](const void *data) -> std::any {
                T msg{};
                std::memcpy(&msg, data, sizeof(T));
                return msg;
            },
        };
    }

    /**
     * @brief 编码：[tag u32][T bytes]
     * @return >0 写入字节数；0 缓冲不足（*size 为所需大小）；-1 类型未注册/any 类型不符
     */
    int32_t Encode(const std::any &pkg, comm::byte *buf, size_t buflen, size_t *size) override
    {
        auto tit = TagOf().find(std::type_index(pkg.type()));
        if (tit == TagOf().end())
            return comm::kCodecDisconnect;  // 未注册类型（同框架 bad_any_cast 语义：-1）
        auto oit = OpsOf().find(tit->second);
        if (oit == OpsOf().end())
            return comm::kCodecDisconnect;

        const TypeOps &ops = oit->second;
        uint32_t need      = kTagSize + ops.size;
        if (buflen < need)
        {
            *size = need;
            return comm::kCodecNeedMoreBuffer;  // 需要更大缓冲（同框架：返回 0 + *size）
        }

        uint32_t tag = tit->second;
        std::memcpy(buf, &tag, kTagSize);
        if (!ops.encode(pkg, buf + kTagSize))
            return comm::kCodecDisconnect;  // any 内容类型不符
        return static_cast<int32_t>(need);
    }

    /**
     * @brief 解码：[tag u32][T bytes]
     * @return >0 消费字节数；0 数据不完整；-1 未知 tag（协议错误）
     */
    int32_t Decode(const comm::byte *buf, size_t buflen, std::any &pkg, size_t * /*size*/) override
    {
        if (buflen < kTagSize)
            return comm::kCodecNeedMoreBuffer;
        uint32_t tag = 0;
        std::memcpy(&tag, buf, kTagSize);
        auto it = OpsOf().find(tag);
        if (it == OpsOf().end())
            return comm::kCodecDisconnect;  // 未知 tag：协议错误（同框架：-1）

        const TypeOps &ops = it->second;
        uint32_t need      = kTagSize + ops.size;
        if (buflen < need)
            return comm::kCodecNeedMoreBuffer;

        pkg = ops.decode(buf + kTagSize);
        return static_cast<int32_t>(need);
    }

    static std::string Name() { return "simplepodcodec"; }

 private:
    /// @brief 每种 POD 类型的编解码操作集
    struct TypeOps
    {
        uint32_t size;  ///< sizeof(T)
        std::function<bool(const std::any &, void *out)> encode;  ///< any -> 字节
        std::function<std::any(const void *data)> decode;         ///< 字节 -> any
    };

    static std::unordered_map<std::type_index, uint32_t> &TagOf()
    {
        static std::unordered_map<std::type_index, uint32_t> tags;
        return tags;
    }

    static std::unordered_map<uint32_t, TypeOps> &OpsOf()
    {
        static std::unordered_map<uint32_t, TypeOps> ops;
        return ops;
    }
};

}  // namespace codec
}  // namespace shm

#endif  // SHM_IPC_SIMPLE_POD_CODEC_HPP_
