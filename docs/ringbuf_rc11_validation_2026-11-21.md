# RingBuffer 并发内存序验证

## 结论

在 GenMC v0.10.0 / LLVM 16.0.6 的 RC11 模型下，10 个当前顺序模型完成检查，未发现安全性质违反；15 个削弱同步的负对照均产生预期的安全反例或非原子数据竞争。没有修改生产头文件，也没有据此删减屏障。

这是有限模型的机械检查证据，不是任意执行、所有平台或整个 SHM 系统的完备证明。普通回绕、共享映射和退出回收的证据见 [上一轮报告](ringbuf_validation_2026-11-21.md)。

## 工具与可复现性

检查器来自 https://github.com/MPI-SWS/genmc ，固定 tag `v0.10.0` / commit `eab70f852eef2d68dd35b64a124c8991e3ec115a`。本机 LLVM 16 不符合最新版 GenMC 的依赖，因此选择其明确支持 LLVM 16 的版本。本轮没有修改检查器源码。

```sh
git clone --branch v0.10.0 --depth 1 https://github.com/MPI-SWS/genmc.git /tmp/shm-genmc
cd /tmp/shm-genmc
autoreconf --install
./configure --with-llvm=/usr/local CXX=g++
make -j4
cd /home/dev/github/shm-cpp
python3 test/modelcheck/run.py /tmp/shm-genmc/genmc /tmp/shm-ring-rc11-results
```

LLVM 路径按本机调整。构建在临时目录，不向系统安装。运行器保留完整命令、输出、预期和实际状态、源代码 SHA256、实际头文件变异补丁。负对照的编译失败、工具错误和超时不会被误认成成功检出。

检查使用 `--rc11 --disable-estimation --disable-sr --disable-ipr --print-error-trace`。未设置调度/上下文截断，模型自身有有限的动作数。`__VERIFIER_assume` 排除需要重试的路径，不产生线程同步边；被排除的执行另计为 blocked，不能拿来声称证明了进展性。

## 性质与代码对应

| 模型 | 实际代码对应 | 检查性质 | 完成执行数 |
|---|---|---|---:|
| reader_wait，初态 Awake | ArmReaderWait / ShouldNotify 原子操作逐项翻译 | 发布完成后，不允许读方可睡眠且写方不通知 | 5 |
| reader_wait，初态 Waiting | 同上，覆盖再次置等待标志 | 同上 | 7 |
| reader_wait，初态 NotifyAlways | 同上，覆盖初始兼容模式 | 同上 | 5 |
| reader_rearm | 两次发布、两次排空/武装及通知计数 | 最后可睡眠且有未读数据时，必须有未消费通知 | 98 |
| publication | BatchWriter 数据写入 + Flush release，对应读方 acquire | 一次发布后两段数据均完整可读 | 1 |
| reuse | 普通写读的游标 load/store，容量抽象为两槽 | 三次写读跨槽复用，内容按序且无非原子竞争 | 9 |
| actual_ring_wait | 直接调用生产 ArmReaderWait / ShouldNotify / TryWrite | 等待与通知性质 | 5 |
| actual_ring_publication | 直接调用生产 TryWrite / ReadExact | 成功读取必须看到发布的字节 | 1 |
| actual_ring_reuse | 生产 1024 字节满环，ReadExact 释放首字节，TryWrite 复用 | 读取与覆写之间有必要的同步，无数据竞争 | 1 |
| actual_ring_batch | 生产 BatchWriter 两次写入、一次 Flush，ReadExact 逐字节读取 | 一次批次发布覆盖两个字节，无数据竞争 | 1 |

合计 133 个完整执行。GenMC 枚举并归约事件图，这些数字不是线程调度次数，也不是随机压测消息数。数量少来自模型小、性质集中、等价执行归约；不表示探索任意长度历史。

C11 翻译保留生产的 relaxed/acquire/release/seq_cst 与 exchange 顺序。两个槽只抽象存储位置复用，不替代容量、数值溢出及字节拷贝的证明。reader_rearm 用 relaxed 原子计数抽象 eventfd 的计数累加/排空，不额外通过通知建立数据 happens-before；没有验证内核 poll、fd 生命周期或通知 syscall 失败。

四个 actual 模型直接包含本仓库 `ringbuf.hpp`，降低翻译错位风险。它们使用已初始化的控制头作为前提，不调用 Init 的页级 memset；GenMC 0.10 的 LLVM opaque-pointer 内存 intrinsic 处理对此不兼容。尝试页级 Init 和 CommitRead 优化生成的 umin 时发生工具错误，未把这些错误记为验证成功。批次中两次 8-bit 写入配合一次 16-bit 读取也触发该版本的 Mixed-size 限制；最终改为两次生产 ReadExact(1)，保持批次整体发布的检查。最终模型避开这些不支持的路径，覆盖范围如表所示。

实际头文件输入使用 `-O1 -std=c++17 -D__cpp_lib_atomic_is_always_lock_free=201603L`。该宏启用 GenMC 自带 atomic shim 中已有的 C++17 属性，使生产 static_assert 正常编译；没有删除 assert，也没有改变 atomic load/store/fence 的实现。直接模型仍依赖 GenMC 的标准库 shim，不等于对真实机器码的验证。

## 负对照：必须能够反驳错误

15 个负对照包括：

- 单轮等待分别删除读方/写方 seq_cst 栅栏，以及把两侧栅栏降为 acq_rel。
- 两轮重新武装分别删除读方/写方栅栏。
- 数据发布分别削弱 release store 或 acquire load。
- 空间复用分别削弱 read_pos 的 release store 或 acquire load。
- 对生产头文件的临时副本分别删除两侧栅栏、削弱发布、削弱回收、削弱空间观察，以及削弱 BatchWriter::Flush，共六项。

所有负对照均被检测；非原子竞争本身已经违反 C++ 数据竞争规则，不需要等到出现可见的数据损坏才能判失败。

最直接的丢唤醒反例：

1. 读方写 Waiting，但在缺少所需栅栏的模型中，复查可读到 write_pos 的旧值 0，因而决定睡眠。
2. 写方发布 write_pos=1，但仍可读到 reader_wait 的旧值 Awake，因而不通知。
3. 两线程结束后，有数据、可睡眠、无通知，安全断言失败。

这是一条弱内存模型允许的事件图，不应解读为 CPU 按日志展示顺序逐行执行。原始 reads-from 及事件图保留在反例日志中。当前双 seq_cst 版本没有该反例。

空间复用的反例则展示：即使写方观察到了新的 read_pos，若缺少对应 acquire 或 release，也没有把消费者的旧数据读取排序在覆写之前，形成非原子竞争。仅仅“看到了新游标”不足以保证安全。

## 检查器警告的处理

GenMC 0.10 对 reader_wait 的原子 store 与原子 exchange 发出 `Write-write race` 警告，并在默认 symmetry reduction / in-place revisiting 下把警告当作错误。这里访问的是原子对象，这种并发写入不构成 C++ 非原子数据竞争。

按工具提示关闭这两项优化后完整检查；保留警告及事件图，不关闭 race detection。负对照的 `Non-atomic race` 仍然正常检出。报告区分这两类信息，没有把工具默认中止当作算法漏洞，也没有直接忽略实际的非原子竞争。

## 仍未证明的范围

- 任意轮数的等待/通知，以及公平调度下的最终进展；本轮把有限丢唤醒坏状态转为安全断言。
- 无限历史到有限模型的归纳/精化证明，以及抽象模型与所有 RingBuf API 的完整对应。
- ISO C++17 所有细节、跨进程 atomics 的平台语义、真实编译器机器码和 ARM 弱内存序硬件。
- eventfd syscall、崩溃、UDS 恢复、业务处理确认等上层协议。
- 独立专家审查或其他检查器的交叉验证。

RC11 是所选工具的语言级修订内存模型，不能把本轮结论直接扩展为所有语言/硬件模型。未来头文件变化后，实际模型自动使用新源码；手工翻译模型的对应关系仍需重新审查。指纹用于识别本次验证对象。

原始证据保存在 [evidence/ringbuf_rc11_2026-11-21](evidence/ringbuf_rc11_2026-11-21/)。

## 后续模型审查

[证明审查报告](ringbuf_proof_audit_2026-11-21.md) 补充了保留首次等待阻塞状态、按生产读取前基线处理半帧的新模型，以及 Z3 局部交叉验证和无界算术保持性检查。原有有限模型的结果和指纹保留，覆盖范围仍按本文解释。
