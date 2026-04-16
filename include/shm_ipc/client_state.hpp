/**
 * @file client_state.hpp
 * @brief 客户端连接状态（client / server 共用）
 */

#ifndef SHM_IPC_CLIENT_STATE_HPP_
#define SHM_IPC_CLIENT_STATE_HPP_

#include "ring_channel.hpp"

namespace shm_ipc {

/**
 * @brief 单条连接的运行状态
 *
 * server 端每个已接入的客户端持有一份；
 * client 端持有自身与 server 的连接状态。
 *
 * server 独有的字段（id、timer_fd、tick、total_read）在 client
 * 端保留默认值，不使用。
 */
struct ClientState
{
    int id = 0;                        ///< 客户端 ID（server 端分配）
    UniqueFd socket_fd;                ///< 连接 socket fd
    DefaultRingChannel channel;        ///< 双向共享内存通道
    int timer_fd   = -1;               ///< 定时器 fd（server 端使用）
    int notify_efd = -1;               ///< 对端写入通知的 eventfd（缓存自 channel）
    int tick       = 0;                ///< 定时器 tick 计数（server 端使用）
    int total_read = 0;                ///< 累计读取消息数
};

}  // namespace shm_ipc

#endif  // SHM_IPC_CLIENT_STATE_HPP_
