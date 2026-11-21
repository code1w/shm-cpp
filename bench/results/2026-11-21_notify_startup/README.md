# 通知初始化假设验证

相同原始1,000,000条bench_app、-O2 -DNDEBUG、server CPU2/client CPU4，每档5次旋转反转，无预热。额外计数仅拦截本程序8字节write，覆盖通道eventfd通知；启动worker和最后Stop写入不计入或只带来极少偏差。计时结束时读取client读环reader_wait状态，0为Awake、1为Waiting、2为NotifyAlways。

baseline为ca766b1；current为当前已简化热路径的代码；arm_early在OnConnection开始调用PrepareWait(ReadPos)，有现存数据时WakeReader确保不丢唤醒；force_client_always诊断性跳过client的ArmReaderWait，让服务端每条回复都写eventfd。生产代码未改动。

结果：force_client_always确实慢至约1.3–1.4M msg/s，服务端每百万消息约百万次write。但baseline/current的全部慢/快样本结束时都已是Awake，通知写入远少于消息数。提前arming对batch256有收益、对batch64无一致收益，不能把它当作已确认的剩余退化修复；初始化优化只能作为独立候选，不能解释全部旧短测差距。

| batch | 版本 | msg/s中位数 | server通知writes中位数 | client通知writes中位数 | 结束reader_mode |
|---|---|---:|---:|---:|---|
| 64 | baseline | 4,449,162 | 17,249 | 15,684 | ['0'] |
| 64 | current | 4,511,059 | 17,224 | 15,698 | ['0'] |
| 64 | arm_early | 4,225,869 | 17,739 | 15,646 | ['0'] |
| 64 | force_client_always | 1,317,306 | 1,000,000 | 11,476 | ['2'] |
| 256 | baseline | 8,780,201 | 628 | 4,163 | ['0'] |
| 256 | current | 7,139,305 | 262 | 4,164 | ['0'] |
| 256 | arm_early | 9,141,945 | 125 | 3,945 | ['0'] |
| 256 | force_client_always | 1,391,879 | 1,000,000 | 3,039 | ['2'] |

初始化确实默认NotifyAlways，首次ArmReaderWait后才进入按需通知；结束状态本身不能确定何时切换，因此保留整个计时窗口的写次数。这里未单独追踪每次状态转换的时间，提前arming的短测收益也可能与批次时序/代码生成交互，不能宣称它是单一根因。

本目录原始logs/CSV、补丁和计数器供复核，主稳态结论见 ../2026-11-21_residual_attribution/README.md。
