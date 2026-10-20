/**
 * @file comm/server.h
 * @brief 服务端抽象接口（shm 与 framework/io 共享）
 *
 * 方法集取自 io 框架 TcpServer。应用代码面向 comm::ITcpServer 编程：
 *   shm::ShmServer  —— 共享内存实现（本仓库）
 *   io::TcpServer   —— TCP 实现（framework/io，待改为继承本接口）
 *
 * uri 约定：shm 实现接受 "shm:///path"、"uds:///path" 或裸 Unix socket 路径；
 * tcp 实现接受框架原有的 "tcp://ip:port" 等形式。
 */

#ifndef COMM_SERVER_H_
#define COMM_SERVER_H_

#include <cstddef>
#include <cstdint>

#include "codec.h"
#include "types.h"

namespace comm {

/**
 * @brief 服务端抽象接口（对应框架 io::TcpServer）
 */
class ITcpServer
{
 public:
    virtual ~ITcpServer() = default;

    // ---- 回调 ----

    virtual void SetVerifyCallback(VerifyCallback cb)             = 0;
    virtual void SetConnectionCallback(ConnectionCallback cb)     = 0;
    virtual void SetCloseCallback(CloseCallback cb)               = 0;
    virtual void SetMessageCallback(AnyMessageCallback cb)        = 0;
    virtual void SetCodecFactory(codec::CodecFactoryCallback cb)  = 0;
    virtual void SetStartFailedCallback(StartFailedCallback cb)   = 0;
    virtual void SetStartSuccessCallback(StartSuccessCallback cb) = 0;

    // ---- 参数 ----

    /// @brief 心跳开关与超时（shm 实现仅记录：断连由 socket EOF 即时感知）
    virtual void SetKeepAlive(bool keepalive, uint32_t io_keepalive_timeout) = 0;

    /// @brief 各连接编码缓冲区（初始大小 / 上限）
    virtual void ResizeBuffer(size_t size, size_t max) = 0;

    /// @brief 监听限流 qps（shm 实现仅记录：本地通信无需限流）
    virtual void SetRateLimit(uint32_t qps) = 0;

    // ---- 生命周期 ----

    /// @brief 开始监听
    /// @return true 成功；失败触发 failed 回调并返回 false
    virtual bool Start() = 0;

    /// @brief 开始监听（tcp 实现支持 ipv6 双栈；shm 实现等同 Start）
    virtual bool StartEx(bool enable_ipv6)
    {
        (void)enable_ipv6;
        return Start();
    }

    /// @brief 停止服务（框架侧待补实现，默认空操作保持接口兼容）
    virtual void Stop() {}
};

}  // namespace comm

#endif  // COMM_SERVER_H_
