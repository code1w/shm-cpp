# 应用层SHM：eventfd vs 忙轮询（2026-11-24）

分支 `feature/shm-busy-poll`，基线提交 `e87c58b`。同一新O3/NDEBUG二进制切换运行参数，SimplePodCodec及生产EventLoop路径；CPU1服务端/CPU3客户端，NUMA1，保留CPU2/4七天长测。每配置预热1秒、测量5秒，3轮交错A/B共18次。单类型codec；batch1全量RTT计时，batch256散列1/64采样。

| 配置 | eventfd RPC/s | 轮询 RPC/s | 配对吞吐比 | 吞吐变化 | eventfd P50 µs | 轮询 P50 µs | eventfd P99.9 µs | 轮询 P99.9 µs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 16B-batch1 | 89,626 | 1,519,301 | 16.91× | +1590.9% | 11.008 | 0.552 | 26.624 | 2.304 |
| 4KiB-batch1 | 73,053 | 341,378 | 4.67× | +367.3% | 13.312 | 2.752 | 29.440 | 11.520 |
| 16B-batch256 | 11,419,996 | 7,540,530 | 0.66× | -33.6% | 9.856 | 16.000 | 27.648 | 39.424 |

吞吐/分位数分别取三轮中位数；比值为逐轮配对比值中位数。RTT不是吞吐倒数，直方图约1.56%桶上界。RPC/s为完整请求响应对。未锁频，5秒短测和三轮观测不能代表尾延迟SLA。

串行小消息明显受等待/唤醒路径影响，轮询大幅提高吞吐并降低RTT；batch256的默认eventfd已有通知摊销，本次轮询反而下降。此对照移除了阻塞等待/数据通知并改变接收驱动节奏，尚未通过剖析分离各项成本，不把batch256下降归因于某个已确定的具体热点。轮询双方各持续占用一个逻辑CPU，不能将结果当成相同CPU资源消耗下的收益。

## 验证

- Release EventLoop默认及轮询生命周期、墓碑替换、取消、定时器、异常恢复和跨线程Stop通过。
- ShmApp默认/服务端轮询/客户端轮询/双方轮询四种配置通过，覆盖同步客户端互通、主动关闭及验证定时器清理。
- RingBuf边界18项（含轮询awake不通知及后续重新武装）、独立字节队列模型、独立进程映射/退出/资源释放测试通过。
- ASan/UBSan EventLoop及上述四种ShmApp配置通过；最新新增Stop测试重新运行通过。
- ASan/UBSan双方轮询18个尺寸/批量冒烟通过，含回绕压力及1MiB批量；日志在sanitizer_smoke，性能数值不用于对照。
- 正式18次运行序号/内容及正常退出、fd/mapping清理检查通过。

`measurement/`为正式原始日志、命令及样本；`calibration/`部分运行与功能测试/编译重叠，明确排除。源码/二进制指纹见sources.sha256。调用契约见[忙轮询文档](../../../docs/shm_busy_poll.md)。

```sh
python3 bench/results/2026-11-24_shm_busy_poll/driver.py /tmp/shm-busy-poll/bench_acceptance /tmp/shm-poll-ab --expanded --server-cpu 1 --client-cpu 3
```

## 后续错误码修订

轮询接口随后改为返回ErrorCode，注册改用nothrow回调节点，分发不再新增try/catch/throw；上表和sources.sha256保留原测量版本，不能将其当作修订后二进制的精确性能结果。修订版验证在error_code_validation目录。
