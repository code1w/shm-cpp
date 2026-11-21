# 最新 SHM/eventfd 与 Unix stream socket 应用层对比

生产核心来自 HEAD `d5e7655`（该提交仅增加文档，核心与前次验收一致）。本轮没有修改 include 下的生产实现。两端真实进程使用同一 SimplePodCodec、EventLoop、消息类型、校验、负载和直方图代码；通过 bench_acceptance 的模板实例选择 SHM 或本轮基准专用 Unix stream 适配器。

这里的 socket 是 AF_UNIX/SOCK_STREAM，不是 TCP 跨机器；SHM 仍使用非阻塞 eventfd 通知。结果是这两条应用实现路径的比较，不是对任意 socket 库或纯硬件传输能力的结论。

## 方法

- GCC 11.2、Release `-O2 -DNDEBUG`，服务端 CPU2、客户端 CPU4，不同物理核心、同 NUMA0。
- 每个配置和每种传输预热1秒、测量10秒；共3轮、42次测量，A/B顺序交错，第二轮配置反向。性能测试串行，没有和 sanitizer 测试重叠。
- 16B/4096B/1048576B 指 POD 消息总长，线格式均为4字节 tag 加 POD；未给 socket 添加额外协议头。
- 客户端连续发一批，收到整批回复后才发下一批。RPC/s 是完成的请求/响应对；吞吐不等于 RTT。
- RTT 从 SendCmd 前到响应回调入口，包含两端 codec、事件循环和传输。16B batch1、1MB和间隔场景全量计时；16B批量约采样1/64，4KB约1/16，散列序号选择样本以覆盖不同批内位置。
- 4096桶直方图报告桶上界，微秒范围分辨率约1.56%；P50/P99/P99.9分别是三轮分位数的中位数。吞吐倍率先按同配置同轮配对计算，再取中位数，不直接用两个中位数相除。
- 未锁频、未排除全部宿主机噪声；closed-loop不覆盖发送前上游排队，也未做coordinated-omission修正。

## 吞吐：Socket 使用整批/当前接收块合并

Socket Encode 直接写入可复用TX队列，收到的数据解析完成后立即合并发送完整回复；处理短读、短写、EAGAIN并按需监听POLLOUT。初始发送与没有读回调触发的发送在连接回调后/派发后flush。没有每条消息新建发送缓冲。SHM按现有SendCmd路径逐条发布。结果包含发送合并和收发缓存生命周期的影响。

| 场景 | SHM RPC/s | Socket RPC/s | 配对吞吐倍率 | SHM 单向载荷 MiB/s | Socket 单向载荷 MiB/s |
|---|---:|---:|---:|---:|---:|
| 16B batch1 | 94,852 | 84,610 | 1.12× | 1.4 | 1.3 |
| 16B batch64 | 4,801,881 | 3,489,489 | 1.38× | 73.3 | 53.2 |
| 16B batch256 | 12,086,019 | 6,575,865 | 1.83× | 184.4 | 100.3 |
| 4KB batch64 | 1,148,864 | 754,440 | 1.52× | 4487.7 | 2947.0 |
| 1MB batch1 | 1,268 | 900 | 1.41× | 1267.6 | 900.4 |
| 1MB batch4 | 2,584 | 961 | 2.69× | 2583.7 | 960.9 |
| 16B batch1，间隔1ms | 920 | 919 | 1.00× | 0.0 | 0.0 |

单向载荷指请求消息体，不含tag；请求+回复的载荷总吞吐是该数值的2倍。避免与旧 bench_socket 把双向字节量标为吞吐的口径混用。间隔1ms场景的速率主要由人为间隔决定，不能当作最大吞吐。

## 逐请求 RTT

| 场景 | SHM P50 µs | Socket P50 µs | SHM P99 µs | Socket P99 µs | SHM P99.9 µs | Socket P99.9 µs |
|---|---:|---:|---:|---:|---:|---:|
| 16B batch1 | 10.37 | 11.65 | 14.59 | 16.00 | 26.37 | 27.65 |
| 16B batch64 | 9.09 | 16.26 | 12.67 | 22.02 | 25.09 | 32.51 |
| 16B batch256 | 9.60 | 30.98 | 14.59 | 43.52 | 25.86 | 51.71 |
| 4KB batch64 | 27.65 | 73.73 | 39.94 | 91.14 | 50.69 | 99.33 |
| 1MB batch1 | 778.24 | 1114.11 | 860.16 | 1179.65 | 999.42 | 1359.87 |
| 1MB batch4 | 1024.00 | 2916.35 | 1572.86 | 4325.38 | 1769.47 | 4653.06 |
| 16B batch1，间隔1ms | 24.32 | 27.14 | 41.98 | 45.06 | 78.85 | 79.87 |

全部响应检查类型、顺序和载荷边界，约1/64做完整载荷校验。完整内容验证在响应计时点后执行，其CPU开销仍包含在负载中。1MB和间隔场景每轮RTT样本较少，P99.9只描述本次观测，不代表高置信度的业务SLA。每轮样本数、吞吐范围及最大RTT保存在summary.json。

## 合并粒度敏感性：只检查16B batch256

不能把整批合并的Socket性能当成所有Socket实现的性能。因此另构建临时变体，在同一个变体二进制中轮换SHM、Socket整批合并、Socket每64条子批合并。消息和应用层batch仍为256；子批只改变内部发送时机。每模式预热1秒、测量5秒、3轮，9次测量；不与主测试混合统计。

| 实现/策略 | RPC/s 中位数 | P50 µs | P99 µs | P99.9 µs |
|---|---:|---:|---:|---:|
| SHM | 11,923,674 | 9.73 | 14.72 | 26.11 |
| Socket整批合并 | 6,455,037 | 31.23 | 46.59 | 52.22 |
| Socket每64条子批合并 | 9,398,599 | 13.95 | 23.04 | 30.98 |

子批合并使Socket的batch256吞吐从6.46百万提升到9.40百万RPC/s，P50从31.23降到13.95µs。对这个更快的Socket配置，SHM的配对吞吐优势约 **26.9%**。主表约83%的优势只对应整批合并策略，不能泛化。

该诊断保留补丁和命令，由check_socket_chunking.py生成临时源码/二进制，没有改写生产代码或主测试的基准源码。仅验证这个配置，不声称已找到所有消息大小下的最优Socket发送策略。

## 内核缓冲与实现范围

标准setsockopt请求4MiB发送和接收缓冲，getsockopt实际均为425984字节（416KiB），内核上限生效。每个方向的SHM环为8MiB，容量不相同。仅在本测试独立socket上探测SO_SNDBUFFORCE/SO_RCVBUFFORCE，均返回EPERM；没有更改全局sysctl。大消息结果包含该环境背压限制，不能直接推广到提高缓冲上限的部署。

Socket基准适配器不是生产TCP框架：只实现本轮codec/收发/生命周期路径，验证和心跳等接口明确不支持。收发缓存和std::any复用粒度也属于实现差异；本轮不把全部差异归因为系统调用或共享内存机制。

## 校准与验证

最初适配器只在接收循环结束后flush回复，复核后改为当前接收块解析完立即flush。此前取消的部分测量保存在calibration_deferred，明确不进入42次主结果。最终版本通过ASan/UBSan+leak冒烟14次测量；1MB路径确认实际触发短写/EAGAIN，并校验所有最终序号与资源基线。

主测试42次和子批诊断9次均正常完成，收发校验无错误，结束后fd回到进程初始基线、memfd映射为0。

## 复现

```sh
cmake -S . -B /tmp/shm-vs-socket -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG"
cmake --build /tmp/shm-vs-socket --target bench_acceptance -j4
python3 bench/compare_socket.py /tmp/shm-vs-socket/bench_acceptance /tmp/shm-socket-results
python3 bench/analyze_socket_comparison.py /tmp/shm-socket-results
python3 bench/check_socket_chunking.py /tmp/shm-socket-results/chunking_diagnostic
```

本目录保存逐进程JSONL、命令、samples.json、summary.json、Socket实际缓冲区和系统调用计数、源码/二进制指纹、sanitizer日志及临时诊断补丁。

结论：当前应用路径下，SHM的批量吞吐和大消息RTT有优势；小消息串行RTT差距较小。Socket发送合并策略显著影响结果，batch256对已测更快子批配置的优势约27%，而非所有Socket都慢约83%。
