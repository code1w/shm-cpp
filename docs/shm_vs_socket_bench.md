# SHM vs Unix Socket 吞吐量对比

## 测试环境

- Linux 3.10.0-1160.el7.x86_64
- 同机进程间通信，client 单向发送，server 接收
- 共享内存模式：8MB SPSC ring buffer + eventfd 通知，**批量写入**（攒满后统一 Flush + 通知）
- Socket 模式：Unix domain socket (SOCK_STREAM)，4MB 收发缓冲区

## 测试方法

两种模式使用完全相同的编码格式：

1. Client 通过 socket 发送 `BenchCmd{payload_size, rounds}` 通知本轮参数
2. Client 循环发送 codec 帧：`[MsgHeader 8B][tag 4B][BenchPayloadHeader 4B][payload NB]`
3. Client 发送 `seq==-1` 的结束标记
4. Server 收到结束标记后发 1 字节 ack
5. Client 从发送第一条消息到收到 ack 的总耗时作为 elapsed

关键区别：
- **SHM**：数据 memcpy 写入 ring buffer，多条消息攒一批后统一 Flush（一次原子 store + 一次 eventfd 通知）
- **Socket**：每条消息一次 `write()` 系统调用

**测量方法学**（2026-09 更新）：
- `taskset` 绑核：server 固定核 2，client 固定核 4，消除进程迁移噪声
- 每档 payload 跑 3 轮取**中位数**
- 一键脚本：`./bench.sh`（参数 `-r` 轮数、`-s/-c` 核号）

> 注意：不绑核的单次运行噪声很大——实测大包档（1MB/2MB）两次运行可差 2 倍，
> 中段（16KB~256KB）因有缓存红利相对稳定。任何异常点必须先复现再分析。

## 测试结果（3 轮中位数）

| payload | SHM msg/s | Socket msg/s | SHM MB/s | Socket MB/s | SHM lat(ns) | Socket lat(ns) | SHM 倍率 |
|--------:|----------:|-------------:|---------:|------------:|------------:|---------------:|---------:|
| 64B | 29,948,620 | 601,281 | 1,827.9 | 36.7 | 33 | 1,663 | 49.8x |
| 256B | 21,385,941 | 638,411 | 5,221.2 | 155.9 | 47 | 1,566 | 33.5x |
| 1KB | 10,477,323 | 619,620 | 10,231.8 | 605.1 | 95 | 1,614 | 16.9x |
| 4KB | 4,058,553 | 500,465 | 15,853.7 | 1,954.9 | 246 | 1,998 | 8.1x |
| 16KB | 1,393,491 | 266,291 | 21,773.3 | 4,160.8 | 718 | 3,755 | 5.2x |
| 64KB | 345,840 | 84,058 | 21,615.0 | 5,253.6 | 2,891 | 11,897 | 4.1x |
| 256KB | 85,840 | 24,557 | 21,460.0 | 6,139.2 | 11,650 | 40,722 | 3.5x |
| 512KB | 41,025 | 11,611 | 20,512.5 | 5,805.5 | 24,376 | 86,125 | 3.5x |
| 1MB | 14,188 | 2,969 | 14,188.1 | 2,968.6 | 70,482 | 336,813 | 4.8x |
| 2MB | 5,360 | 1,463 | 10,720.0 | 2,925.4 | 186,567 | 683,527 | 3.7x |

> MB/s 按有效载荷（payload_size）计算，不含帧头开销。

## 关键发现

### 1. SHM 在小消息上优势巨大：50x

64B 消息时 SHM 达到 **3000 万 msg/s**（摊薄延迟 33ns），Socket 仅 60 万 msg/s（1.7μs）。
SHM 批量写入将数百条消息的 memcpy 合并为一次原子 store + 一次 eventfd 通知，
摊薄了每条消息的固定开销；Socket 每条消息必须经历一次完整的 `write()`
系统调用（用户态→内核态→用户态），小包时 msg/s 几乎是一条 ~60 万的水平线。

### 2. 随 payload 增大，倍率从 50x 降至 3.7x

| payload | SHM 倍率 | 主要瓶颈 |
|--------:|---------:|----------|
| 64B | 49.8x | 每消息固定开销（系统调用 vs 原子操作） |
| 1KB | 16.9x | 开始受 memcpy 带宽影响 |
| 16KB | 5.2x | memcpy 带宽成为主要因素 |
| 256KB | 3.5x | 两者都受 DRAM 带宽制约 |
| 2MB | 3.7x | ring 容量限制（8MB 仅容纳 4 条消息） |

小消息时瓶颈在系统调用开销，SHM 通过批量大幅避免；大消息时瓶颈转移到内存带宽，
socket 是 2 次内核拷贝、shm 是 1 次 memcpy，差距收敛到拷贝次数之比。

### 3. SHM 带宽在 16KB-512KB 达到峰值平台 ~21 GB/s

16KB~512KB 区间稳定在 20~22 GB/s：消息工作集落在 L2/L3 缓存内，
写者刚 memcpy 的数据还在热缓存中即被读者消费（cache-to-cache 传输）。
2MB 时降至 ~10.7 GB/s：ring 容量 8MB 仅能容纳约 4 条消息，writer 频繁等待
reader 释放空间，且大 memcpy 超出缓存直达 DRAM。

### 4. Socket 在所有 payload 上带宽均不超过 6.2 GB/s

Socket 峰值带宽 6.1 GB/s（256KB 时），仅为 SHM 峰值的 28%。Unix socket 每次
传输需要两次内核拷贝（send buffer + recv buffer），大幅消耗内存带宽；
小包时还要再叠加每消息一次的系统调用税。

### 5. SHM 小消息摊薄延迟仅 33ns

| payload | SHM lat | Socket lat | 延迟比 |
|--------:|--------:|-----------:|-------:|
| 64B | 33ns | 1.7μs | 50x |
| 1KB | 95ns | 1.6μs | 17x |
| 64KB | 2.9μs | 11.9μs | 4.1x |
| 2MB | 187μs | 684μs | 3.7x |

SHM 64B 延迟 33ns 是批量摊薄后的均值——实际每条消息仅需一次 memcpy，
真实的端到端延迟取决于批次大小和 Flush 频率（ping-pong 延迟见 bench_shm）。

## 复现方法

```bash
./build.sh          # 构建
./bench.sh          # 绑核 + 3 轮中位数，直接输出对比表

# 或手动单跑一组：
./build/server --bench &  sleep 0.5 && ./build/client --bench      # SHM
./build/socket_server &   sleep 0.5 && ./build/socket_client       # Socket 基线
```
