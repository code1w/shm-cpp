# Echo Benchmark: Unix Socket vs Shared Memory

## 测试环境

- Linux 3.10.0, GCC 11.2.0, `-O2`
- `fork()` + `socketpair()` 单进程内 server/client
- Ping-pong echo：client 发一条 → server 原样回送 → client 收到后再发下一条
- 每组预热 1000 轮，不计入统计

## 测试结果

### Unix Socket

```
  msg_size     rounds     total_us     avg_rtt_ns  throughput_MB/s
        64     100000      1100469          11005            11.09
       256     100000       964557           9646            50.62
      1024      50000       457744           9155           213.34
      4096      50000       498028           9961           784.34
     65536      10000       316986          31699          3943.40
    524288       5000       638133         127627          7835.36
```

### Shared Memory (RingChannel, 零拷贝 + busy-poll)

```
  msg_size     rounds     total_us     avg_rtt_ns  throughput_MB/s
        64     100000        60406            604           202.08
       256     100000        56999            570           856.65
      1024      50000        43492            870          2245.38
      4096      50000        85818           1716          4551.79
     65536      10000       172507          17251          7246.10
    524288       5000      1614429         322886          3097.07
```

### 对比

| msg_size | Socket RTT (ns) | SHM RTT (ns) | SHM 加速比 |
|----------|-----------------|---------------|-----------|
| 64B      | 11,005          | 604           | **18.2x** |
| 256B     | 9,646           | 570           | **16.9x** |
| 1KB      | 9,155           | 870           | **10.5x** |
| 4KB      | 9,961           | 1,716         | **5.8x**  |
| 64KB     | 31,699          | 17,251        | **1.8x**  |
| 512KB    | 127,627         | 322,886       | 0.4x      |

## 优化说明

SHM benchmark 采用两项优化：

1. **零拷贝读（`peek_read`/`commit_read`）**：server echo 时直接从 read ring 取得数据指针，写入 write ring，省去一次 512KB 的 `memcpy`。
2. **去掉 `notify_peer()`**：busy-poll 模式下双方已在自旋等待，无需 eventfd 系统调用唤醒。

## 结论

- **小消息（64B ~ 1KB）**：共享内存 RTT 比 Unix socket 低 **10 ~ 18 倍**。socket 每次 `write`/`read` 需要用户态-内核态切换和内核 buffer 拷贝；共享内存直接在用户态 `memcpy` 到 mmap 区域，无系统调用开销。
- **中等消息（4KB ~ 64KB）**：共享内存仍有 **1.8 ~ 5.8 倍** 优势。
- **大消息（512KB）**：socket 更快（约 2.5x）。内核 socket 对大块数据传输有页面级优化，而 ring buffer 的 `memcpy` 在大消息下 cache 压力大。该场景下 memcpy 本身成为绝对瓶颈，非算法层面可优化。
- **适用场景**：共享内存方案在低延迟、小消息高频场景（金融交易、实时控制、游戏服务器内部通信）优势显著；大消息批量传输场景下 socket 仍具竞争力。
