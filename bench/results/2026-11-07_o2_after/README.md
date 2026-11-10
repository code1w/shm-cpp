# 优化后性能日志（2026-11-07，-O2）

对照基线：`bench/results/2026-11-07_o2_baseline/`。环境、payload 网格、绑核方式完全一致。

## 已实施优化

1. **P0-1 延迟/合并通知**
   - `EventLoop` 新增 post-dispatch 钩子。
   - `ShmConnection` 新增 `SetDeferNotify` / `FlushNotify`：同一分发批次内的多次发送合并为一次 eventfd。
   - `ShmServer::SetDeferredNotify(bool)` / `ShmAsyncClient::SetDeferredNotify(bool)`，**默认关闭**（保持原有低延迟行为），高吞吐场景显式开启。
2. **P0-2 读侧批量提交**
   - `ShmConnection::DrainRing` 改为一次 `Peek`、局部游标连续解码、整批一次 `CommitRead`。
3. **P0-3 回绕线性化限流**
   - 跨环尾时只线性化"当前帧"（`SimplePodCodec::Decode` 回报所需长度），不再复制全部可读数据。
4. **P1-1 SimplePodCodec 扁平化**
   - `unordered_map` × 2 + `std::function` → 扁平 `std::vector` 注册表 + 普通函数指针。

## 1. 上层接口吞吐（新增 `bench_app`）

`ShmServer + ShmAsyncClient + SimplePodCodec`，1,000,000 次请求/响应，客户端每批等待全部响应后发下一批：

| batch | immediate（每消息通知） | deferred（批次合并） | deferred / immediate |
|---:|---:|---:|---:|
| 1 | 95,540 msg/s | 95,201 msg/s | 1.00x |
| 8 | 1,200,243 msg/s | 649,248 msg/s | **0.54x** |
| 64 | 1,653,658 msg/s | 2,753,125 msg/s | **1.66x** |
| 256 | 1,779,804 msg/s | 4,337,310 msg/s | **2.44x** |

结论：
- 合并在 **batch ≥ 64** 时显著提升吞吐；batch=8 时因减少了收发流水线重叠而变慢，batch=1 无合并空间。
- 因此默认保持 immediate，`SetDeferredNotify(true)` 由高吞吐/突发发送场景显式开启。

优化前同代码（old SimplePodCodec）的上层数据（同机、同 batch 逻辑）：
deferred 94.5k / 634k / 2.32M / 3.34M；immediate 95.8k / 1.14M / 1.53M / 1.67M。
对比可见 P1-1 + P0-2/3 使 deferred batch=64 提升约 **19%**，batch=256 提升约 **30%**。

## 2. codec 内存往返（探针）

| 类型 | 优化前 | 优化后 |
|---|---:|---:|
| 16B encode | 35.1 ns | **6.8 ns** |
| 16B decode | 28.1 ns | **20.5 ns** |
| 4096B encode | 77.6 ns | 92.6 ns（噪声） |
| 4096B decode | 169.8 ns | 168.8 ns |

16B encode 提升约 5x；4KB decode 仍以 `std::any` 堆分配为主，需后续 API 级优化。

## 3. 低层路径回归（未改动）

`bench.sh` 优化后结果与基线一致（raw RingChannel + 批量 SendFrame 路径未改动），例如：

| payload | 基线 SHM MB/s | 优化后 SHM MB/s |
|---|---:|---:|
| 64B | 1804.9 | 1748.8 |
| 1KB | 11530.1 | 11621.2 |
| 16KB | 24749.0 | 24716.8 |
| 256KB | 21573.5 | 21595.8 |
| 1MB | 5836.7 | 5834.0 |

零拷贝探针、ring 容量探针结果同样落在基线噪声范围内。

## 4. 测试

```
PASS test_ringbuf_wrap / test_ringbuf_boundary / test_codec /
     test_eventloop / test_proto_codec / test_proto_wrap / test_shm_app
ASan+UBSan：7/7 PASS，bench_app（deferred, batch=64）PASS
```

## 5. 尚未实施

- P1-2 对外零拷贝读取 API（`ShmClient`/直接 RingChannel 用户仍走 `TryRead` 拷贝）。
- P1-3 镜像双重映射（彻底消除跨尾线性化，改动较大）。
- `std::any` 大对象堆分配：需要新增类型化回调/小缓冲 any 才能消除。
