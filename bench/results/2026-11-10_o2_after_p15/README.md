# 优化后性能日志 P1-5（2026-11-10，-O2）

对照：`../2026-11-07_o2_baseline/`、`../2026-11-07_o2_after/`、`../2026-11-07_o2_after_p13/`、`../2026-11-09_o2_after_p14/`。

## 本阶段改动：消除 SendCmd 双拷贝

1. `comm::codec::Codec` 新增非纯虚 `EncodedSize(const std::any&) const`（默认 -1）。
2. `SimplePodCodec` 实现 `EncodedSize`（tag + sizeof(T)）。
3. `ShmConnection::SendCmd`：`need >= 256B` 时直接 `StartBatch → Reserve(need) → Encode → CommitReserve → FlushNoNotify`，把
   `any → encode_buf_ → ring` 两次拷贝降为 **any → ring 一次**；<256B 仍走 encode_buf_（缓存命中，避免批量器开销）。
4. `ChannelBatchWriter::FlushNoNotify`：发布但不通知，保留 `defer_notify_` 的合并通知语义。
5. `SimplePodCodec::Decode`：改为 `emplace<T>` + 直接 memcpy 到 any 存储，去掉 "ring → 栈上 T → any" 的临时拷贝。
6. `bench_app` 新增 `--big`（4KB）与 `--huge`（1MB）模式。

## 1. 上层小消息（Heartbeat 16B）

| batch | immediate | deferred | deferred 上一版(P1-4) |
|---:|---:|---:|---:|
| 1 | 97,560 | 95,168 | 94,389 |
| 8 | 1,300,191 | 678,955 | 680,338 |
| 64 | 1,589,065 | 3,386,236 | 3,227,971 |
| 256 | 1,702,961 | **6,039,311** | 5,143,030 |

deferred batch256 再提升约 17%（Decode 去掉临时拷贝；小消息编码仍走 encode_buf）。

## 2. 上层 4KB（`--big`）

| batch | immediate | deferred |
|---:|---:|---:|
| 8 | 613,868 msg/s / 2407 MB/s | 353,420 / 1386 |
| 64 | **765,630 / 3002** | 563,172 / 2209 |
| 256 | 684,671 / 2685 | 660,550 / 2590 |

对比 P1-4：immediate batch64 2.45 → **3.00 GB/s（+22%）**；deferred batch256 2.21 → 2.59 GB/s（+17%）。

## 3. 上层 1MB（`--huge`，旧路径不支持）

旧实现 `encode_buf_` 上限 1MB，`1MB + tag` 无法编码，`SendCmd` 直接失败；直写 ring 后支持到 `max_write_size`（4MB）。

| batch | immediate | deferred |
|---:|---:|---:|
| 1 | 1,251 msg/s / 1251 MB/s | 1,281 / 1281 |
| 2 | 1,982 / 1982 | 1,501 / 1501 |
| 4 | **2,569 / 2569** | 1,566 / 1566 |

回声往返仍有 4 次 1MB 拷贝（客户端 encode、服务端 decode/encode、客户端 decode），因此有效单向 ~2.6 GB/s；下一步要靠零拷贝视图或并行分片继续提升。

## 4. 低层 raw 路径

`bench.sh` 与 P1-3 一致（本次只改上层 codec/SendCmd）：1KB ~13.5k MB/s、16KB ~25k MB/s、1MB ~5.5k MB/s。

## 5. 验证

```
测试 7/7 PASS
ASan+UBSan 7/7 PASS + bench_app small/big/huge PASS
```

## 6. 说明

- `Reserve` 依赖镜像映射才能跨环尾连续；镜像失败（退化映射）时跨尾 `Reserve` 返回 nullptr，自动回退到 encode_buf 路径。
- `SendCmd` 的直写上限为 `RingChannel::max_write_size`（Capacity/2，默认 4MB）；超过仍走原路径。
- 小消息（<256B）保留原路径，避免 Reserve/Commit 的固定开销。
