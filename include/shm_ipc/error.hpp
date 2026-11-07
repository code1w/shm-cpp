/**
 * @file error.hpp
 * @brief 公共错误码定义（公共 API 不抛异常）
 *
 * 库内所有显式异常均已移除，统一返回 ErrorCode。系统调用失败时可通过
 * 可选的 int* sys_errno 出参取得原始 errno，便于日志定位。
 *
 * 例外：标准库的内存分配失败（std::bad_alloc）与线程创建失败
 * （std::system_error）仍可能在内部抛出，但会在公共入口被捕获并转换为
 * kNoMemory / kSystem；用户回调抛出的异常保持原有语义（codec 回滚后
 * 继续传播，EventLoop 回收墓碑后继续传播）。
 */

#ifndef SHM_IPC_ERROR_HPP_
#define SHM_IPC_ERROR_HPP_

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace shm {

/** @brief 统一错误码 */
enum class ErrorCode : int32_t
{
    kOk = 0,          ///< 成功
    kInvalidArgument, ///< 参数非法（空出参、路径过长等）
    kSystem,          ///< 系统调用失败（errno 见 sys_errno 出参）
    kTimeout,         ///< EAGAIN/EWOULDBLOCK（握手超时等）
    kPeerClosed,      ///< 对端关闭（EOF/EPIPE/ECONNRESET/ENOTCONN）
    kProtocol,        ///< 协议错误（tag 不符、非法控制消息、CTRUNC）
    kSizeMismatch,    ///< 对端环尺寸与本地 Capacity 不一致
    kNotInitialized,  ///< 在默认构造/moved-from 对象上操作
    kMultipleFds,     ///< 一条 SCM_RIGHTS 消息携带多个 fd
    kNoMemory,        ///< 已捕获的 std::bad_alloc
};

/** @brief 错误码名称（静态字符串，无分配） */
inline const char *ErrorCodeName(ErrorCode ec) noexcept
{
    switch (ec)
    {
        case ErrorCode::kOk:              return "ok";
        case ErrorCode::kInvalidArgument: return "invalid argument";
        case ErrorCode::kSystem:          return "system error";
        case ErrorCode::kTimeout:         return "timeout";
        case ErrorCode::kPeerClosed:      return "peer closed";
        case ErrorCode::kProtocol:        return "protocol error";
        case ErrorCode::kSizeMismatch:    return "size mismatch";
        case ErrorCode::kNotInitialized:  return "not initialized";
        case ErrorCode::kMultipleFds:     return "multiple fds";
        case ErrorCode::kNoMemory:        return "out of memory";
    }
    return "unknown error";
}

/** @brief errno → ErrorCode 归类 */
inline ErrorCode ErrnoToErrorCode(int err) noexcept
{
    switch (err)
    {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return ErrorCode::kTimeout;
        case EPIPE:
        case ECONNRESET:
        case ENOTCONN:
            return ErrorCode::kPeerClosed;
        default:
            return ErrorCode::kSystem;
    }
}

/**
 * @brief 生成可读错误描述（仅失败路径使用，会分配 std::string）
 * @param ec        错误码
 * @param sys_errno 系统 errno，0 表示不附加
 */
inline std::string ErrorCodeMessage(ErrorCode ec, int sys_errno = 0)
{
    std::string msg = ErrorCodeName(ec);
    if (sys_errno != 0)
    {
        msg += ": ";
        msg += std::strerror(sys_errno);
    }
    return msg;
}

}  // namespace shm

#endif  // SHM_IPC_ERROR_HPP_
