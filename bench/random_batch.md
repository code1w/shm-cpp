# 随机 batch 应用层 SHM vs Unix socket

`bench_acceptance` 新增 `--batch-min MIN --batch MAX --seed SEED`。每批均匀随机选择MIN到MAX，收到整批回复后再发送下一批；不是固定在途窗口，也不是实际生产流量回放。省略 `--batch-min` 保留原固定batch行为。

正式对照涵盖15个消息尺寸、small/burst两组范围和SHM/eventfd、SHM/busy、Unix stream socket三种传输。small范围通常1–8，burst通常1–64；64KiB burst上限32、128KiB上限16、512KiB/1MiB上限4。512KiB/1MiB small上限2。沿用既有批量和在途字节限制，线格式另含4字节tag。

```sh
cmake -S . -B /tmp/shm-busy-poll -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/shm-busy-poll --target bench_acceptance test_random_batch -j4
/tmp/shm-busy-poll/test_random_batch
python3 bench/compare_random_batch.py /tmp/shm-busy-poll/bench_acceptance /tmp/shm-random-ab --server-cpu 1 --client-cpu 3
python3 bench/analyze_random_batch.py /tmp/shm-random-ab
```

默认3轮、每配置预热1秒/测量5秒，共270次正式测量；模式次序按轮次轮换、第二轮尺寸次序反向。`--seconds`及`--rounds`可调整。`--smoke`运行一次90配置短测，不能用于正式性能分析。可用 `--sizes 16 4096 1048576` 做定向验证。输出目录不得含已有samples.json。

预热和正式测量使用独立PRNG序列；预热结束重置为本轮配对种子，三个模式从同一batch序列开始。正式输出验证前256批的哈希相同，并记录全量批大小计数、完成批数和消息数，检查sum(batch*count)==完成RPC数。按时间结束，快模式会处理更长的随机序列前缀；大尺寸低吞吐的实际批量分布偏差保存在原始计数中。

随机生成每批一次；常见2的幂范围使用掩码，非2的幂范围使用取模（64位PRNG映射偏差可忽略）。固定数组记录时间戳和批量计数，热路径无新增内存分配、逐消息随机数或逐消息日志。批量统计和前缀校验只在批边界进行，日志在结束时输出。辅助测试用禁用异常的编译选项构建；新代码不使用try/catch/throw。

所有尺寸使用同一种类型的SimplePodCodec和同一EventLoop/消息校验框架。RTT从SendCmd前到回复回调入口；≤4KiB散列采样约1/64，8–32KiB约1/16，≥64KiB全量采样。抽样按请求序号散列覆盖批内位置。固定直方图约1.56%桶上界；保留P50/P99/P99.9和样本数，短测大消息的P99.9仅是少数观测值，不能据此承诺SLA。

均匀分布以“批”为单位，按“请求”统计的RTT会更偏向大批；不能把均匀批次解释为均匀请求。closed-loop没有业务空闲间隔/上游排队，不模拟生产到达率，未做coordinated-omission修正。本轮主要比较可变聚合量下的饱和吞吐及应用层RTT。

Socket为基准专用AF_UNIX/SOCK_STREAM适配器，保留接收块/派发后合并发送，内核缓冲约416KiB；SHM每方向环8MiB。busy模式双方各持续占用一个CPU，不是相同空闲CPU成本的比较。控制fd检查仍采用当前默认256轮，读取/业务回调不返回时控制事件可能被延后。没有锁频或排除宿主机噪声。

分析输出三种传输中位数及逐轮配对的SHM相对Socket提升，负值保留；不跨不同尺寸/批量合并成一个通用提升百分比。原始命令、逐进程日志、batch计数、随机前缀校验和二进制指纹可复查。
