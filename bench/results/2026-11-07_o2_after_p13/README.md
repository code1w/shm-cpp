# 优化后性能日志 P1-2/P1-3（2026-11-07，-O2）

对照：
- 基线：`../2026-11-07_o2_baseline/`
- P0+P1-1：`../2026-11-07_o2_after/`

## 本阶段改动

1. **P1-2 `ShmClient::PeekMessage` / `CommitMessage`**
   - 复用 `ReadMessage` 的等待逻辑，直接返回 ring 内数据段；调用方处理后 `CommitMessage(total)`。
   - `test_shm_app` 新增 part1b 零拷贝读验证。
2. **P1-3 数据区镜像双重映射**
   - `RingHeader` 后填充到页边界（`kRingDataOffset = 4096`），数据区页对齐。
   - `MmapRegion::MapMirrored`：完整文件映射一次 + 数据区（file offset 4096）在 `base + file_size` 再映射一次，数据区 VA 连续两圈。
   - `RingChannel` 读写改走 `TryWriteContig/TryReadContig/ReadExactContig/PeekContig`：跨环尾单次 memcpy、`Peek` 永远单段。
   - 镜像失败（VA 不足 / Capacity 未页对齐等）自动退化为普通映射，原有两段路径仍可用。
   - `BatchWriter`/`Reserve` 在镜像模式下允许跨尾单次直写。

## 1. 端到端吞吐（`bench.sh`，两次运行）

| payload | 基线 SHM MB/s | P1-3 run1 | P1-3 run2 | 相对基线 |
|---|---:|---:|---:|---:|
| 64B | 1804.9 | 1824.3 | 1865.0 | ~0 |
| 512B | 8965.8 | 10275.4 | 11342.1 | **+15% ~ +27%** |
| 1KB | 11530.1 | 12268.2 | 13570.4 | **+6% ~ +18%** |
| 2KB | 14254.9 | 14671.0 | 15902.0 | +3% ~ +12% |
| 4KB | 17842.9 | 18994.8 | 20143.6 | **+6% ~ +13%** |
| 16KB | 24749.0 | 24953.5 | 25019.5 | ~+1% |
| 64KB | 22139.0 | 22734.0 | 22769.6 | +3% |
| 256KB | 21573.5 | 22152.2 | 22169.7 | +3% |
| 512KB | 20781.8 | 21857.8 | 21844.6 | **+5%** |
| 768KB | 18151.0 | 19857.7 | 19753.7 | **+9%** |
| 1MB | 5836.7 | 5565.2 | 5553.5 | −5%（大包噪声/内存带宽） |
| 2MB | 5994.0 | 5419.6 | 5429.4 | −9%（大包方差大，待更多轮确认） |

结论：512B–768KB 区间收益稳定（跨尾单次拷贝 + 单段 Peek）；1MB/2MB 仍由单核内存带宽主导，镜像映射无收益，当前两次结果略低于基线，需在同一进程内做 A/B 才能排除环境漂移。

## 2. 上层接口（`bench_app`）

| batch | immediate | deferred |
|---:|---:|---:|
| 1 | 97,749 msg/s | 94,656 msg/s |
| 8 | 1,216,853 msg/s | 664,412 msg/s |
| 64 | 1,649,248 msg/s | 2,761,952 msg/s |
| 256 | 1,772,810 msg/s | 3,952,347 msg/s |

与 P0+P1-1 阶段基本一致（该路径原本已通过 `Peek` 零拷贝；DrainRing 已批量提交），变化在噪声范围内。

## 3. ping-pong（`bench_shm`）

512KB RTT 304µs，与基线 181–300µs 的波动区间重叠；ping-pong harness 本身 4 次拷贝 + 跨核同步，不是镜像映射的主要受益场景。

## 4. 验证

```
测试 7/7 PASS
ASan+UBSan 7/7 PASS + bench_app(deferred,64) PASS
```

新增断言：`test_codec` Test 9 在跨尾 512B 帧上验证 `ReadMirrored()` 且小 `MaxBufSize` 下仍零拷贝成功。

## 5. 剩余事项

- 大对象 `std::any` 的堆分配（4KB decode ~169ns）需要类型化回调 / 小缓冲 any 才能消除。
- 1MB/2MB 大包：受单核写带宽限制；NT store 与大页在本机实测无收益/不可用。
- 如需进一步降低 ping-pong 延迟，可增加基于 `Peek/CommitRead` 的零拷贝 echo 基准并配套 API。
