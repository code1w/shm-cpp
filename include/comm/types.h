/**
 * @file comm/types.h
 * @brief 通信层公共类型定义（shm 与 framework/io 共享的接口层）
 *
 * 本头文件是两种传输实现（shm 共享内存 / io 框架 TCP）的公共协议：
 * 应用代码只依赖 comm:: 类型编程，即可在 shm::ShmServer 与
 * io::TcpServer 之间自由切换实现。
 *
 * 回调签名、枚举取值与 io 框架 io/defines.h（tcp 部分）保持一致，
 * 保证框架侧改为继承接口时的兼容。
 *
 * 对应关系：
 *   io::TcpConnPtr           -> comm::TcpConnPtr
 *   io::ConnectionCallback   -> comm::ConnectionCallback
 *   io::AnyMessageCallback   -> comm::AnyMessageCallback
 *   io::CloseCallback        -> comm::CloseCallback
 *   io::VerifyCallback       -> comm::VerifyCallback
 *   io::ConnectErrorCallback -> comm::ConnectErrorCallback
 *   io::CloseType            -> comm::CloseType
 *   io::TcpConnType          -> comm::TcpConnType
 *   io::TcpConnection::States  -> comm::TcpConnStates
 *   io::TcpAsyncClient::States -> comm::TcpClientStates
 */

#ifndef COMM_TYPES_H_
#define COMM_TYPES_H_

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace comm {

using byte = uint8_t;  ///< 与框架 base::byte 一致

// ---------------------------------------------------------------------------
// codec 返回值约定（数值与 io/defines.h 的 CODEC_* 宏一致，
// 使现有框架 codec 无需修改即可复用）
// ---------------------------------------------------------------------------

inline constexpr int32_t kCodecNeedMoreBuffer = 0;   ///< CODEC_NEEDMOREDAFFER：需要更多数据或更大内存
inline constexpr int32_t kCodecDisconnect     = -1;  ///< CODEC_DISCONNECT：编解码错误，断开连接
inline constexpr int32_t kCodecSkipPackage    = -2;  ///< CODEC_SKIPPACKAGE：编码丢包 / 解码方向为心跳包

/// @brief 连接关闭类型（与 io::CloseType 一致）
enum class CloseType
{
    Unknow          = 0,
    Active          = 1,  ///< 主动关闭
    Passive         = 2,  ///< 被动关闭
    KeepLiveTimeOut = 3,
};

/// @brief 连接类型（与 io::TcpConnType 一致）
enum TcpConnType
{
    Unknown        = 0,
    TcpConn_Task   = 1,  ///< server 端接受的连接
    TcpConn_Client = 2,  ///< client 端主动发起的连接
};

/// @brief 连接状态（取值对应框架 TcpConnection::States；State() 以 int32_t 返回）
struct TcpConnStates
{
    enum
    {
        verifing    = 0,  ///< 验证中
        established = 1,  ///< 已建立
        closeing    = 2,  ///< 关闭中
        closeed     = 3,  ///< 已关闭
    };
};

/// @brief 客户端状态（取值对应框架 TcpAsyncClient::States；State() 以 int32_t 返回）
struct TcpClientStates
{
    enum
    {
        established = 0,  ///< 已建立
        connecting  = 1,  ///< 连接中
        closeing    = 2,  ///< 关闭中
        close       = 3,  ///< 已关闭
    };
};

using TimeAction = std::function<void(int64_t)>;  ///< 对应 io::TimeAction
using TimerID    = int64_t;                       ///< 对应 base::TimerID

class ITcpConnection;

using TcpConnPtr  = std::shared_ptr<ITcpConnection>;  ///< 对应 io::TcpConnPtr
using TcpConnWptr = std::weak_ptr<ITcpConnection>;    ///< 对应 io::TcpConnWptr

using ConnectionCallback   = std::function<void(const TcpConnPtr &)>;                    ///< 对应 io::ConnectionCallback
using CloseCallback        = std::function<void(const TcpConnPtr &)>;                    ///< 对应 io::CloseCallback
using VerifyCallback       = std::function<void(const TcpConnPtr &)>;                    ///< 对应 io::VerifyCallback
using AnyMessageCallback   = std::function<void(const TcpConnPtr &, const std::any &)>;  ///< 对应 io::AnyMessageCallback
using ConnectErrorCallback = std::function<void(bool, const std::string &)>;             ///< 对应 io::ConnectErrorCallback
using StartFailedCallback  = std::function<void(std::string)>;                           ///< 对应 io::TcpServer::StartFailedCallback
using StartSuccessCallback = std::function<void()>;                                      ///< 对应 io::TcpServer::StartSuccessCallback

}  // namespace comm

#endif  // COMM_TYPES_H_
