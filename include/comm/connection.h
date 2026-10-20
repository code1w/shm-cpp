/**
 * @file comm/connection.h
 * @brief 连接抽象接口（shm 与 framework/io 共享）
 *
 * 方法集取自 io 框架 tcp::TcpConnection 的应用层可用部分。
 * 应用代码只持有 comm::TcpConnPtr，即可在两种传输实现间切换：
 *   shm::ShmConnection      —— 共享内存实现（本仓库）
 *   io::tcp::TcpConnection  —— TCP 实现（framework/io，待改为继承本接口）
 */

#ifndef COMM_CONNECTION_H_
#define COMM_CONNECTION_H_

#include <any>
#include <cstdint>
#include <string>

#include "codec.h"
#include "types.h"

namespace comm {

/**
 * @brief 连接抽象接口（对应框架 tcp::TcpConnection 的应用层 API）
 */
class ITcpConnection
{
 public:
    virtual ~ITcpConnection() = default;

    // ---- 数据面 ----

    /// @brief 发送消息包（codec 编码后写入传输层），成功返回 true
    virtual bool SendCmd(const std::any &pkg) = 0;

    /// @brief 主动关闭连接（异步收尾，之后触发 close 回调）
    virtual void Close() = 0;

    // ---- 状态 ----

    /// @brief 连接状态（取值见 comm::TcpConnStates）
    virtual int32_t State() const = 0;
    /// @brief 连接 ID
    virtual uint64_t Id() const = 0;
    /// @brief 对端地址描述（tcp 为 "ip:port"，shm 为 "pid:<n>"）
    virtual std::string RemoteAddr() const = 0;
    /// @brief 关闭类型
    virtual CloseType GetCloseType() const = 0;

    // ---- 上下文 ----

    virtual void SetContext(std::any context)      = 0;
    virtual const std::any &Context() const        = 0;
    virtual void SetRpcContext(std::any context)   = 0;
    virtual const std::any &RpcContext() const     = 0;

    // ---- 验证 / 心跳 / 缓冲 ----

    /// @brief 启动验证超时定时器
    /// @param expired_ms  超时时间（毫秒）
    /// @param interval_ms 重复间隔（shm 实现暂按一次性处理，传 0）
    virtual void StartVerifyTimer(int64_t expired_ms, int64_t interval_ms,
                                  TimeAction action) = 0;

    /// @brief 开启/关闭应用层心跳响应
    virtual void SetKeepAlive(bool keepalive) = 0;

    /// @brief 调整编码缓冲区（初始大小 / 上限）
    virtual void ResizeBuffer(size_t size, size_t max) = 0;

    /// @brief 当前连接的 codec
    virtual codec::CodecPtr GetCodec() = 0;
};

}  // namespace comm

#endif  // COMM_CONNECTION_H_
