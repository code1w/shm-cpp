# SHM vs Unix Socket 吞吐量对比

## 测试环境

- Linux 3.10.0-1160.el7.x86_64
- 同机进程间通信，client 单向发送，server 接收
- 共享内存模式：8MB SPSC ring buffer + eventfd 通知
- Socket 模式：Unix domain socket (SOCK_STREAM)，4MB 收发缓冲区

## 测试协议

两种模式使用完全相同的编码格式和流程：

1. Client 通过 socket 发送 `BenchCmd{payload_size, rounds}` 通知本轮参数
2. Client 循环发送 codec 帧：`[MsgHeader 8B][tag 4B][BenchPayloadHeader 4B][payload NB]`
3. Client 发送 `seq==-1` 的结束标记
4. Server 收到结束标记后发 1 字节 ack
5. Client 从发送第一条消息到收到 ack 的总耗时作为 elapsed

唯一区别：SHM 模式数据写入 ring buffer（零拷贝），Socket 模式数据通过 `write()`/`read()` 系统调用传输。

## 测试结果

| payload | SHM msg/s | Socket msg/s | SHM MB/s | Socket MB/s | SHM lat(ns) | Socket lat(ns) | SHM 倍率 |
|--------:|----------:|------------:|---------:|------------:|------------:|--------------:|---------:|
| 64B | 1,962,485 | 556,459 | 119.8 | 34.0 | 510 | 1,797 | 3.53x |
| 256B | 1,422,149 | 613,780 | 347.2 | 149.8 | 703 | 1,629 | 2.32x |
| 1KB | 1,889,882 | 573,002 | 1,845.6 | 559.6 | 529 | 1,745 | 3.30x |
| 4KB | 1,481,142 | 429,015 | 5,785.7 | 1,675.8 | 675 | 2,331 | 3.45x |
| 16KB | 851,040 | 151,170 | 13,297.5 | 2,362.0 | 1,175 | 6,615 | 5.63x |
| 64KB | 221,704 | 59,840 | 13,856.5 | 3,740.0 | 4,511 | 16,711 | 3.70x |
| 256KB | 67,610 | 24,737 | 16,902.5 | 6,184.2 | 14,791 | 40,426 | 2.73x |
| 512KB | 32,115 | 12,066 | 16,057.7 | 6,033.1 | 31,138 | 82,876 | 2.66x |
| 1MB | 10,354 | 3,212 | 10,353.8 | 3,212.4 | 96,583 | 311,295 | 3.22x |
| 2MB | 4,169 | 1,351 | 8,337.8 | 2,701.7 | 239,873 | 740,282 | 3.09x |

> MB/s 按有效载荷（payload_size）计算，不含帧头开销。

## 关键发现

### 1. SHM 在所有消息大小上都快 2.3-5.6x

- **小消息（64B-1KB）**：SHM 快 2.3-3.5x。SHM 延迟 500-700ns，Socket 延迟 1.6-1.8μs。瓶颈在每条消息的固定开销（原子操作 vs 系统调用）。
- **中等消息（4-16KB）**：SHM 优势最大，达 **3.5-5.6x**（16KB 时峰值 5.63x）。Socket 的内核拷贝开销开始成为主要瓶颈。
- **大消息（64KB-2MB）**：SHM 稳定快 2.7-3.7x。两者都受内存带宽制约，但 Socket 多了两次内核拷贝。

### 2. SHM 吞吐在 256KB 时达到峰值 ~17 GB/s

SHM 的 MB/s 在 16KB-256KB 区间达到峰值 13-17 GB/s，之后随 payload 增大反而下降（2MB 时降至 ~8.3 GB/s）。原因：

- ring buffer 容量仅 8MB，2MB payload 意味着环中最多同时容纳 4 条消息，writer 频繁等待 reader 释放空间
- 大 payload 的 `memcpy` 超出 L3 缓存，触发 DRAM 访问

### 3. Socket 在大消息时延迟急剧增长

| payload | SHM lat | Socket lat | 延迟比 |
|--------:|--------:|-----------:|-------:|
| 64B | 510ns | 1.8μs | 3.5x |
| 16KB | 1.2μs | 6.6μs | 5.6x |
| 512KB | 31μs | 83μs | 2.7x |
| 2MB | 240μs | 740μs | 3.1x |

Socket 每条 2MB 消息延迟 740μs，是 SHM 的 3.1x。Unix socket 每次 `write()`/`read()` 需要用户态→内核态→用户态两次数据拷贝，大消息放大了这一开销。

### 4. SHM msg/s 在小消息时接近 200 万/秒

SHM 在 64B 时达到 msg/s 峰值 ~196 万/秒（延迟 510ns），而 Socket 峰值仅 ~61 万/秒（256B 时）。SHM 的优势来自：
- 无系统调用：直接 memcpy 到共享内存
- 无内核介入：eventfd 仅用于通知，数据路径完全在用户态
- 缓存友好：小消息的 ring buffer 热区始终在 L1/L2 缓存中

### 5. 超大消息（1-2MB）两者吞吐都大幅下降

| payload | SHM MB/s | Socket MB/s |
|--------:|---------:|------------:|
| 256KB | 16,902 | 6,184 |
| 1MB | 10,354 | 3,212 |
| 2MB | 8,338 | 2,702 |

SHM 从 256KB 的 17 GB/s 降至 2MB 的 8.3 GB/s（-51%），Socket 从 6.2 GB/s 降至 2.7 GB/s（-56%）。两者都受 DRAM 带宽和缓存失效影响，但 SHM 仍保持 3x 左右的优势。

## 复现方法

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

# SHM bench
./server --bench &  sleep 0.5 && ./client --bench

# Socket bench
./socket_server &  sleep 0.5 && ./socket_client
```
