# 优化后性能日志 P1-4（2026-11-09，-O2）

对照：`../2026-11-07_o2_baseline/`、`../2026-11-07_o2_after/`、`../2026-11-07_o2_after_p13/`。

## 本阶段改动：大对象 `std::any` 原地复用

1. `SimplePodCodec::Decode` 改为**写入已有 any**：
   - 同类型：`std::any_cast<T>(&out)` 成功 → 直接 `*p = msg`，复用 any 内部存储；
   - 类型变化：`out = msg`，重新分配一次。
2. `ShmConnection::DrainRing` 在整次 drain 中复用**同一个 `std::any`**，同类型消息不再逐条构造/析构 any 与堆分配。
3. `bench_app` 新增 `--big`（4KB POD）模式，客户端/服务端也复用发送/回包 any。
4. `test_shm_app` Part 0 验证大对象连续两次 decode 复用同一 any 存储（地址不变）。

## 1. 上层吞吐：小消息（Heartbeat 16B）

| batch | immediate（本次） | deferred（本次） | deferred 上次(P1-3) | 变化 |
|---:|---:|---:|---:|---:|
| 1 | 94,506 | 94,389 | 94,656 | ~0 |
| 8 | 1,230,322 | 680,338 | 664,412 | +2% |
| 64 | 1,609,982 | **3,227,971** | 2,761,952 | **+17%** |
| 256 | 1,891,557 | **5,143,030** | 3,952,347 | **+30%** |

Immediate batch256 1.77M → 1.89M（+7%）；deferred 提升更明显，因为通知合并后 any 构造/析构成为主要剩余 CPU 成本。

## 2. 上层吞吐：大消息（4KB POD，`--big`）

| batch | immediate | deferred |
|---:|---:|---:|
| 1 | 77,930 msg/s / 305.6 MB/s | 78,242 / 306.8 |
| 8 | **522,774 / 2050.1** | 314,733 / 1234.2 |
| 64 | **624,071 / 2447.3** | 518,363 / 2032.8 |
| 256 | **717,036 / 2811.9** | 563,680 / 2210.5 |

大消息场景 **immediate 全面优于 deferred**（batch≥8），因为 4KB memcpy 是主体，延迟通知减少了收发流水线重叠；再次验证 deferred 只适合小包高吞吐。

## 3. Decode any 复用微基准

| 类型 | fresh any | reused any | 收益 |
|---|---:|---:|---:|
| 16B POD | 31.2 – 82.3 ns | **9.5 – 22.6 ns** | **3.3 – 3.6x** |
| 4096B POD | 111 – 265 ns | 106 – 208 ns | 最多 ~22%（4KB 拷贝主导） |

（同一二进制内 fresh/reused 对比；绝对值随 CPU 频率波动。）

## 4. 低层路径回归

`bench.sh` 结果与 P1-3 一致（P1-4 只影响上层 `SimplePodCodec`/`DrainRing`），例如 1KB ~13.5k MB/s、16KB ~25k MB/s、1MB ~5.5k MB/s。

## 5. 验证

```
测试 7/7 PASS（含 part0 big-pod any reuse）
ASan+UBSan 7/7 PASS + bench_app small/big PASS
```

## 6. 注意

- 回调收到的 `const std::any&` 只在本次回调期间有效（原本也是如此）；不要在回调外保存引用。
- 类型交替（如 Heartbeat/ClientMsg 混合）时 any 类型变化，仍会分配；同类型流式消息收益最大。
