/**
 * @file simple_pod_codec.hpp
 * @brief 通用 POD 消息 codec（C++17，std::any 消息模型）
 *
 * 实现 comm::codec::Codec 接口，帧格式：[tag u32][T bytes]，
 * 复用 SHM_IPC_REGISTER_POD 的 TypeTag 注册体系，消息以 std::any 携带值。
 *
 * 热路径使用扁平注册表 + 函数指针（避免 unordered_map 与 std::function
 * 的双重间接），注册表在进程启动阶段一次性构建、之后只读。
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
#include <typeindex>
#include <type_traits>
#include <typeinfo>
#include <vector>

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
 *
 * @note RegisterPod 必须在进程启动阶段、任何并发收发之前调用；同一
 *       类型/tag 只注册一次。
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
        static_assert(std::is_trivially_copyable_v<T>, "SimplePodCodec::RegisterPod<T>: T must be trivially copyable");
        static_assert(std::is_default_constructible_v<T>,
                      "SimplePodCodec::RegisterPod<T>: T must be default constructible");
        Registry().push_back(RegistryEntry{
            std::type_index(typeid(T)),
            TypeTag<T>::value,
            sizeof(T),
            // encode：从 any 取出 T 的字节
            [](const std::any& pkg, void* out) -> bool {
                const T* p = std::any_cast<T>(&pkg);
                if (!p)
                {
                    return false;
                }
                std::memcpy(out, p, sizeof(T));
                return true;
            },
            // decode：直接写入 any 内部存储，避免 "栈上临时 T + 再拷贝"。
            // 同类型时复用已有存储；类型变化时原地构造一次。
            [](const void* data, std::any& out) {
                auto* p = std::any_cast<T>(&out);
                if (!p)
                {
                    out.emplace<T>();
                    p = std::any_cast<T>(&out);
                }
                std::memcpy(p, data, sizeof(T));
            },
        });
    }

    /**
     * @brief 编码：[tag u32][T bytes]
     * @return >0 写入字节数；0 缓冲不足（*size 为所需大小）；-1 类型未注册/any 类型不符
     */
    int32_t Encode(const std::any& pkg, comm::byte* buf, size_t buflen, size_t* size) override
    {
        const RegistryEntry* e = FindByType(pkg.type());
        if (!e)
        {
            return comm::kCodecDisconnect;  // 未注册类型（同框架 bad_any_cast 语义）
        }

        const uint32_t need = kTagSize + e->size;
        if (buflen < need)
        {
            *size = need;
            return comm::kCodecNeedMoreBuffer;  // 需要更大缓冲（同框架：返回 0 + *size）
        }

        std::memcpy(buf, &e->tag, kTagSize);
        if (!e->encode(pkg, buf + kTagSize))
        {
            return comm::kCodecDisconnect;  // any 内容类型不符
        }
        return static_cast<int32_t>(need);
    }

    /**
     * @brief 解码：[tag u32][T bytes]
     * @return >0 消费字节数；0 数据不完整（*size 为当前帧所需总长）；-1 未知 tag
     */
    int32_t Decode(const comm::byte* buf, size_t buflen, std::any& pkg, size_t* size) override
    {
        if (buflen < kTagSize)
        {
            if (size)
            {
                *size = kTagSize;  // 告诉上层至少还需要 tag
            }
            return comm::kCodecNeedMoreBuffer;
        }
        uint32_t tag = 0;
        std::memcpy(&tag, buf, kTagSize);
        const RegistryEntry* e = FindByTag(tag);
        if (!e)
        {
            return comm::kCodecDisconnect;  // 未知 tag：协议错误（同框架：-1）
        }

        const uint32_t need = kTagSize + e->size;
        if (buflen < need)
        {
            if (size)
            {
                *size = need;  // 当前帧完整长度，供 DrainRing 精确线性化
            }
            return comm::kCodecNeedMoreBuffer;
        }

        e->decode(buf + kTagSize, pkg);
        return static_cast<int32_t>(need);
    }

    /**
     * @brief 编码后精确长度（供传输层直接写 ring，消除双拷贝）
     * @return kTagSize + sizeof(T)；类型未注册返回 -1
     */
    [[nodiscard]]
    int32_t EncodedSize(const std::any& pkg) const override
    {
        const RegistryEntry* e = FindByType(pkg.type());
        if (!e)
        {
            return -1;
        }
        return static_cast<int32_t>(kTagSize + e->size);
    }

    static std::string Name() { return "simplepodcodec"; }

 private:
    using EncodeFn = bool (*)(const std::any&, void*);
    /// decode 写入已有的 any：同类型时原地复用其存储
    using DecodeFn = void (*)(const void*, std::any&);

    /// @brief 每种 POD 类型的编解码操作集
    struct RegistryEntry
    {
        std::type_index type;  ///< 类型
        uint32_t tag;          ///< 类型标签
        uint32_t size;         ///< sizeof(T)
        EncodeFn encode;       ///< any -> 字节
        DecodeFn decode;       ///< 字节 -> 已有 any（原地复用）
    };

    /// @brief 进程级扁平注册表（启动阶段构建，之后只读）
    static std::vector<RegistryEntry>& Registry()
    {
        static std::vector<RegistryEntry> entries;
        return entries;
    }

    static const RegistryEntry* FindByType(const std::type_info& type) noexcept
    {
        const std::type_index key(type);
        for (const auto& e : Registry())
        {
            if (e.type == key)
            {
                return &e;
            }
        }
        return nullptr;
    }

    static const RegistryEntry* FindByTag(uint32_t tag) noexcept
    {
        for (const auto& e : Registry())
        {
            if (e.tag == tag)
            {
                return &e;
            }
        }
        return nullptr;
    }
};

}  // namespace codec
}  // namespace shm

#endif  // SHM_IPC_SIMPLE_POD_CODEC_HPP_
