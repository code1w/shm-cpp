/**
 * @file comm/client.h
 * @brief 客户端抽象接口（shm 与 framework/io 共享）
 *
 * 应用代码面向接口编程，即可在两种传输实现间切换：
 *   ITcpAsyncClient:  shm::ShmAsyncClient / io::TcpAsyncClient（待改为继承本接口）
 *   ITcpClient:       shm::ShmClient      / io::tcp::Client<T>（待改为继承本接口）
 *
 * Connect 的 uri 约定：
 *   shm 实现："shm:///path"、"uds:///path" 或裸 Unix socket 路径
 *   tcp 实现：框架原有的 "tcp://ip:port" 等形式
 *
 * 注意 Connect 的异步语义差异：tcp 实现为真异步（结果走回调），
 * shm 实现为同步阻塞（本地握手，Connect 返回时状态已确定）；
 * 两种实现都保证成功触发 connection 回调、失败触发 error 回调，
 * 应用层不应假设 Connect 返回时回调尚未触发。
 */

#ifndef COMM_CLIENT_H_
#define COMM_CLIENT_H_

#include <any>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "codec.h"
#include "types.h"

namespace comm {

/**
 * @brief 异步客户端抽象接口（对应框架 io::TcpAsyncClient）
 */
class ITcpAsyncClient
{
 public:
    virtual ~ITcpAsyncClient() = default;

    // ---- 回调 ----

    virtual void SetVerifyCallback(VerifyCallback cb)             = 0;
    virtual void SetConnectionCallback(ConnectionCallback cb)     = 0;
    virtual void SetMessageCallback(AnyMessageCallback cb)        = 0;
    virtual void SetCloseCallback(CloseCallback cb)               = 0;
    virtual void SetConnectErrorCallback(ConnectErrorCallback cb) = 0;
    virtual void SetCodecFactory(codec::CodecFactoryCallback cb)  = 0;

    // ---- 参数 ----

    virtual void SetContext(std::any context)       = 0;
    virtual const std::any &Context() const         = 0;
    virtual void SetAutoReconnect(bool autoreconn)  = 0;
    virtual void ResizeBuffer(size_t size, size_t max) = 0;

    // ---- 状态 ----

    /// @brief 客户端状态（取值见 comm::TcpClientStates）
    virtual int32_t State() const = 0;
    /// @brief 连接 ID，未连接返回 0
    virtual uint64_t Id() const = 0;

    // ---- 数据面 ----

    /// @brief 发起连接（结果通过 connection / error 回调通知）
    virtual void Connect(const std::string_view &uri) = 0;

    /// @brief 发送消息包，成功返回 true
    virtual bool SendCmd(const std::any &pkg) = 0;

    /// @brief 主动关闭连接
    virtual void Close() = 0;
};

/**
 * @brief 同步裸字节流客户端抽象接口（对应框架 io::tcp::Client<T>）
 *
 * 不经 codec 的原始字节流收发，适用于命令行工具、脚本式调用等
 * 不需要事件循环的场景。
 */
class ITcpClient
{
 public:
    virtual ~ITcpClient() = default;

    /// @brief 连接服务端（同步）
    virtual bool Connect(const std::string_view &uri) = 0;

    /// @brief 连接服务端（tcp 形式；shm 实现恒返回 false，仅作编译兼容）
    virtual bool Connect(const std::string_view &ip, uint16_t port) = 0;

    /**
     * @brief 发送原始字节流
     * @return 成功返回 len；失败返回 -1
     */
    virtual int SendMessage(const char *message, const int len) = 0;

    /**
     * @brief 阻塞读取：直到读到数据或对端关闭
     * @return 实际读取字节数（>0）；对端关闭返回 0；未连接返回 -1
     */
    virtual int ReadMessage(char *buf, size_t bsize) = 0;

    /// @brief 关闭连接
    virtual void Close() = 0;

    /// @brief 是否已连接
    virtual bool Connected() const = 0;
};

}  // namespace comm

#endif  // COMM_CLIENT_H_
