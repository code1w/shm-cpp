/**
 * @file messages.hpp
 * @brief 应用层 POD 消息定义
 */

#ifndef SHM_IPC_MESSAGES_HPP_
#define SHM_IPC_MESSAGES_HPP_

#include <shm_ipc/codec.hpp>

#include <cstdint>

/// 服务端心跳消息
struct Heartbeat
{
    int32_t client_id;
    int32_t seq;
    int64_t timestamp;
};
SHM_IPC_REGISTER_POD(Heartbeat, 0x48425454)  // "HBTT"

/// 客户端消息
struct ClientMsg
{
    int32_t seq;
    int32_t tick;
    int64_t timestamp;
    uint32_t payload_len;  ///< 有效载荷实际长度
    char payload[4080];    ///< 载荷数据区
};
SHM_IPC_REGISTER_POD(ClientMsg, 0x434C4D47)  // "CLMG"

#endif  // SHM_IPC_MESSAGES_HPP_
