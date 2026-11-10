# 优化前性能基线（2026-11-07）

## 环境与构建

- Xeon Gold 6226R @ 2.90GHz，64 vCPU，Linux 3.10.0
- 构建：`CMAKE_BUILD_TYPE=Release` + `CMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"`
- `bench.sh`：taskset server 核 2 / client 核 4，3 轮中位数
- `bench_shm` / `bench_socket`：未绑核（绝对值有 2–3x 波动，主要用于趋势对比）
- payload 网格：64B 128B 256B 512B 1K 2K 4K 8K 16K 32K 64K 128K 256K 384K 512K 768K 1M 1.5M 2M

## 文件

| 文件 | 内容 |
|---|---|
| `bench_sh_run1.txt` / `run2` | `bench.sh` 完整输出（SHM vs socket 端到端吞吐） |
| `bench_shm_run1.txt` / `run2` | SHM ring 回声 RTT（11 档） |
| `bench_socket_run1.txt` / `run2` | Unix socket 回声 RTT（11 档） |
| `bench_batch.txt` | 逐条写入 vs BatchWriter（原子操作成本） |
| `bench_codec.txt` | 裸 TryWrite vs Send/SendPod + FrameReader |
| `perf_stat.txt` | `perf stat bench_shm` |
| `probes.txt` | eventfd/批量、ring 容量、零拷贝消费、NT 存储探针 |

## 关键基线数字

### 端到端吞吐（`bench_sh_run2.txt`，3 轮中位数）

| payload | SHM MB/s | Socket MB/s | SHM 倍率 |
|--------:|---------:|------------:|---------:|
| 64B | 1804.9 | 39.3 | 45.9x |
| 1KB | 11530.1 | 601.8 | 19.2x |
| 16KB | 24749.0 | 4421.1 | 5.6x |
| 256KB | 21573.5 | 6342.8 | 3.4x |
| 1MB | 5836.7 | 3177.1 | 1.8x |
| 2MB | 5994.0 | 3256.8 | 1.8x |

（`run1` 与 `run2` 小包档波动约 ±10–15%，大包 <3%。）

### 探针关键结论

- eventfd `write()`：**472 ns/次**
- 64B：纯 TryWrite 32.4 ns/msg；每次通知 512.7 ns/msg；Batch100 单条 18.9 ns/msg
- 16KB：`ReadExact` 拷贝 12.58 GB/s vs `Peek+CommitRead` 零拷贝 24.29 GB/s（**1.93x**）
- 1MB：read 拷贝 5.24 GB/s vs 零拷贝 5.94 GB/s（+13%）
- ring 8MB→64MB：1MB 吞吐 4.86→4.95 GB/s（**容量不是瓶颈**）
- NT store 无收益（1MB：memcpy 5.9 vs NT 5.5 GB/s）
- `SimplePodCodec`：16B encode/decode 35.1/28.1 ns；4096B 77.6/169.8 ns

### 优化前已知瓶颈（对应实施计划）

1. 每消息一次 eventfd 通知（P0-1）
2. `DrainRing` 每消息 `Peek` + `CommitRead`，以及回绕时线性化全部可读数据（P0-2/P0-3）
3. `SimplePodCodec` 的 map/`std::function`/`std::any` 开销（P1-1）

优化完成后在 `bench/results/<日期>_after/` 保存同口径日志，并与此目录逐项对比。
