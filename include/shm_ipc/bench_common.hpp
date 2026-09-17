/**
 * @file bench_common.hpp
 * @brief 吞吐量测试公共定义（client/server 共用）
 */

#ifndef SHM_IPC_BENCH_COMMON_HPP_
#define SHM_IPC_BENCH_COMMON_HPP_

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <time.h>
#include <unistd.h>

#include "codec.hpp"
#include "pod_codec.hpp"

namespace shm {

/// Bench 专用类型标签 "BENC"
inline constexpr uint32_t kBenchTag = 0x42454E43;

/// Bench payload 头部（seq==-1 表示结束标记）
struct BenchPayloadHeader
{
    int32_t seq;
};

/// 吞吐量测试控制命令（client→server，通过 socket 传递）
struct BenchCmd
{
    uint32_t payload_size;
    int32_t  rounds;  ///< 0 = 全部测试结束
};

/// 单组测试参数
struct BenchCase
{
    uint32_t payload_size;
    int32_t  rounds;
};

/// 默认测试用例
inline constexpr BenchCase kBenchCases[] = {
    {       64, 500000},
    {      256, 500000},
    {     1024, 200000},
    {     4096, 200000},
    {    16384, 100000},
    {    65536,  50000},
    {   262144,  20000},
    {   524288,  10000},
    {  1048576,   5000},
    {  2097152,   2000},
};

/// 计算单帧总字节数 = MsgHeader + tag + BenchPayloadHeader + payload_size
inline uint32_t BenchFrameSize(uint32_t payload_size)
{
    return kMsgHeaderSize + kTagSize
         + static_cast<uint32_t>(sizeof(BenchPayloadHeader)) + payload_size;
}

/// @brief 循环读满 len 字节（处理 stream socket 短读与 EINTR）
/// @return true 读满，false 对端关闭或读出错
inline bool ReadFull(int fd, void *buf, std::size_t len)
{
    auto *p = static_cast<char *>(buf);
    std::size_t got = 0;
    while (got < len)
    {
        ssize_t n = ::read(fd, p + got, len - got);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;  // 对端关闭
        got += static_cast<std::size_t>(n);
    }
    return true;
}

/// @brief 循环写满 len 字节（处理 stream socket 短写与 EINTR）
/// @return true 写满，false 写出错（含对端关闭导致的 EPIPE）
inline bool WriteFull(int fd, const void *buf, std::size_t len)
{
    auto *p = static_cast<const char *>(buf);
    std::size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::write(fd, p + sent, len - sent);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 获取单调时钟纳秒时间戳
inline uint64_t NowNs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

/// 打印 bench 表头
inline void PrintBenchHeader()
{
    std::printf("=== End-to-end throughput (client -> server) ===\n\n");
    std::printf("%12s %10s %10s %12s %12s %10s\n",
                "payload", "rounds", "time(ms)", "msg/s", "MB/s", "lat(ns)");
}

/// 计算并打印单行 bench 结果
inline void PrintBenchRow(uint32_t payload_size, int32_t received,
                          uint64_t elapsed_ns)
{
    double ms    = static_cast<double>(elapsed_ns) / 1e6;
    double msg_s = static_cast<double>(received) / (static_cast<double>(elapsed_ns) / 1e9);
    double mb_s  = msg_s * payload_size / (1024.0 * 1024.0);
    double lat   = static_cast<double>(elapsed_ns) / received;

    std::printf("%12u %10d %10.1f %12.0f %12.1f %10.0f\n",
                payload_size, received, ms, msg_s, mb_s, lat);
}

}  // namespace shm

#endif  // SHM_IPC_BENCH_COMMON_HPP_
