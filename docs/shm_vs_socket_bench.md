# SHM vs Unix Socket 吞吐量对比

## 测试环境

- Linux 3.10.0-1160.el7.x86_64
- 同机进程间通信，client 单向发送，server 接收
- 共享内存模式：8MB SPSC ring buffer + eventfd 通知，**批量写入**（攒满后统一 Flush + 通知）
- Socket 模式：Unix domain socket (SOCK_STREAM)，4MB 收发缓冲区

## 测试协议

两种模式使用完全相同的编码格式：

1. Client 通过 socket 发送 `BenchCmd{payload_size, rounds}` 通知本轮参数
2. Client 循环发送 codec 帧：`[MsgHeader 8B][tag 4B][BenchPayloadHeader 4B][payload NB]`
3. Client 发送 `seq==-1` 的结束标记
4. Server 收到结束标记后发 1 字节 ack
5. Client 从发送第一条消息到收到 ack 的总耗时作为 elapsed

关键区别：
- **SHM**：数据 memcpy 写入 ring buffer，多条消息攒一批后统一 Flush（一次原子 store + 一次 eventfd 通知）
- **Socket**：每条消息一次 `write()` 系统调用

## 测试结果

| payload | SHM msg/s | Socket msg/s | SHM MB/s | Socket MB/s | SHM lat(ns) | Socket lat(ns) | SHM 倍率 |
|--------:|----------:|------------:|---------:|------------:|------------:|--------------:|---------:|
| 64B | 34,041,869 | 586,428 | 2,077.8 | 35.8 | 29 | 1,705 | 58.1x |
| 256B | 18,775,327 | 505,626 | 4,583.8 | 123.4 | 53 | 1,978 | 37.1x |
| 1KB | 9,405,269 | 478,954 | 9,184.8 | 467.7 | 106 | 2,088 | 19.6x |
| 4KB | 3,821,239 | 418,635 | 14,926.7 | 1,635.3 | 262 | 2,389 | 9.1x |
| 16KB | 1,324,080 | 250,888 | 20,688.7 | 3,920.1 | 755 | 3,986 | 5.3x |
| 64KB | 331,695 | 84,603 | 20,730.9 | 5,287.7 | 3,015 | 11,820 | 3.9x |
| 256KB | 82,991 | 25,552 | 20,747.7 | 6,387.9 | 12,050 | 39,137 | 3.2x |
| 512KB | 40,291 | 11,101 | 20,145.6 | 5,550.5 | 24,819 | 90,082 | 3.6x |
| 1MB | 14,098 | 2,830 | 14,098.2 | 2,830.4 | 70,931 | 353,306 | 5.0x |
| 2MB | 2,975 | 1,641 | 5,949.9 | 3,281.6 | 336,140 | 609,452 | 1.8x |

> MB/s 按有效载荷（payload_size）计算，不含帧头开销。

## 关键发现

### 1. SHM 在小消息上优势巨大：58x

64B 消息时 SHM 达到 **3400 万 msg/s**（延迟 29ns），Socket 仅 59 万 msg/s（延迟 1.7μs）。SHM 批量写入将数百条消息的 memcpy 合并为一次原子 store + 一次 eventfd 通知，摊薄了每条消息的固定开销。而 Socket 每条消息必须经历一次完整的 `write()` 系统调用（用户态→内核态→用户态）。

### 2. 随 payload 增大，倍率从 58x 降至 1.8x

| payload | SHM 倍率 | 主要瓶颈 |
|--------:|---------:|----------|
| 64B | 58.1x | 每消息固定开销（系统调用 vs 原子操作） |
| 1KB | 19.6x | 开始受 memcpy 带宽影响 |
| 16KB | 5.3x | memcpy 带宽成为主要因素 |
| 256KB | 3.2x | 两者都受 DRAM 带宽制约 |
| 2MB | 1.8x | ring 容量限制（8MB 仅容纳 4 条消息） |

小消息时瓶颈在系统调用开销，SHM 通过批量大幅避免；大消息时瓶颈转移到内存带宽，两者差距缩小。

### 3. SHM 带宽在 64KB-256KB 时达到峰值 ~20 GB/s

SHM 的 MB/s 在 16KB-256KB 区间稳定在 20 GB/s，之后随 payload 增大下降（2MB 时降至 ~6 GB/s）。原因：

- ring buffer 容量仅 8MB，2MB payload 意味着环中最多同时容纳 4 条消息，writer 频繁等待 reader 释放空间
- 大 payload 的 `memcpy` 超出 L3 缓存，触发 DRAM 访问

### 4. Socket 在所有 payload 上带宽均不超过 6.4 GB/s

Socket 峰值带宽 6.4 GB/s（256KB 时），仅为 SHM 峰值的 31%。Unix socket 每次传输需要两次内核拷贝（send buffer + recv buffer），大幅消耗内存带宽。

### 5. SHM 小消息延迟仅 29ns

| payload | SHM lat | Socket lat | 延迟比 |
|--------:|--------:|-----------:|-------:|
| 64B | 29ns | 1.7μs | 58x |
| 1KB | 106ns | 2.1μs | 20x |
| 64KB | 3.0μs | 11.8μs | 3.9x |
| 2MB | 336μs | 609μs | 1.8x |

SHM 64B 延迟 29ns 是批量摊薄后的均值——实际每条消息仅需一次 memcpy，真正的延迟取决于批次大小和 Flush 频率。

## 复现方法

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

# SHM bench（批量写入模式）
./server --bench &  sleep 0.5 && ./client --bench

# Socket bench
./socket_server &  sleep 0.5 && ./socket_client
```
