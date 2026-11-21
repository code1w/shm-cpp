# RingBuffer 深层验证：性质、反例与证据

本轮针对数值回绕、顺序参考模型和真实跨进程边界，没有修改生产实现。适用前提为 SPSC、同一时刻单一活跃 BatchWriter、初始化完成后共享。本次平台为 Linux x86-64 / GCC 11.2，页大小 4096。

## 1. 游标回绕的性质

游标是模 2^64 的无符号值，而非永不溢出的整数。正确不变量是模差 `(write_pos - read_pos) <= Capacity`，不能用存储值的 `read_pos <= write_pos` 表示。

令逻辑累计写入和消费量为 W、R，若 `0 <= W-R <= Capacity < 2^64`，无符号模差可准确表示占用。Capacity 是 2 的幂并整除 2^64，数值回绕不改变物理索引。旧读位置快照限制写方继续推进，因此只能导致保守拒绝。

这是算术推导，未独立证明所有 acquire/release 执行都会维持前提；后者仍需并发模型检查。

`test_ringbuf_model` 从 UINT64_MAX 前 1、8、半容量、一容量和容量+1 字节开始，验证未发布批次跨数值回绕仍不可读、Cancel 不发布、填满/拒绝/读空、一次通知和过期等待基线。此外精确构造 Reserve 提交到游标 0、错误长度拒绝、局部检查点回滚，以及读方已释放空间时旧 writer 保守拒绝而新 writer 成功。

普通映射覆盖容量 1024/4096/8192；镜像覆盖 4096/8192。本测试不宣称支持其他页大小。

## 2. 顺序参考模型

参考模型以 deque 保存已发布字节、vector 保存未发布字节，逻辑累计量在有限测试内不回绕；不复制环索引或双段拷贝算法。每步检查内容、可读量、空闲量、共享游标、批次 PendingBytes/Count/FreeBytes。

混合普通写、批次写、Reserve/提交、Flush/重复 Flush、Cancel、析构发布、Rewind、移动、精确读取、部分读取和 Peek/Commit。消费者可在批次存续时读已发布数据；模型保存构造时的 read 快照，验证真实的保守空间契约。

每种映射/容量执行 12 个固定种子，分别从 0 和 UINT64_MAX 附近开始，每个序列 2000 步，共 240,000 步。长度偏向 0、1、半容量、半容量+1、容量和容量+1。失败打印种子、步骤、容量、映射、初始游标和表达式。传入打印的种子即可重放对应序列：

```sh
/tmp/shm-ring-proof/test_ringbuf_model 23063
```

为验证测试具备反驳能力，`test/ringbuf_mutation_check.py` 在临时头文件副本注入提前发布、Cancel 发布、错误拒绝数值回绕、接受错误 Reserve 提交长度，以 Release/NDEBUG 编译，要求四个变体全部被检测。脚本不修改生产代码。该结果只证明对这四种错误敏感，不是完整变异覆盖率。

顺序模型不覆盖所有并发交错；模型本身也可能有错误。

## 3. 真实跨进程边界

`test_ringbuf_process` 分两层：

- 子进程通过不同地址重新映射同一个 memfd，父进程校验序号和完整确定性载荷。UDS 控制三种边界：发布先于等待、等待先于发布、起跑后竞争。竞争场景在发布后没有控制 UDS 同步。普通/镜像均覆盖，初始游标跨 UINT64_MAX。
- fork 时通道尚不存在，双方通过生产 `RingChannel::CreateRingChannel` 和真实 SCM_RIGHTS 获取对端 fd。三个新会话覆盖正常发布并通知、未 Flush 就退出、已发布但尚未通知就退出。

子进程用 `_exit` 跳过析构，避免错误地把 BatchWriter 析构 Flush 当作崩溃行为。父进程读 UDS EOF 后验证未发布数据不可见、已发布数据完整可读、本端映射在对端退出后仍有效。析构后 fd 返回 EBADF、映射查询返回 ENOMEM；生产通道的 memfd 映射及 fd 数回到会话前基线。下次握手从空通道开始，没有旧数据。

阻塞有超时或 alarm，子进程全部 waitpid 回收。这是 RingBuf/RingChannel 及内核 EOF 验证，不是新增的 ShmServer/ShmAsyncClient 自动重连集成测试。已有应用层 EOF 回归另行覆盖 owner 排空行为。

关键边界：发布和通知不是原子事务。进程在两者之间退出时，eventfd 可以无事件，而环中存在已发布数据；因此 UDS EOF 路径排空环是必要的。此处不承诺业务完成确认、exactly-once 或崩溃重放。

## 复现与检查

```sh
cmake -S . -B /tmp/shm-ring-proof -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/shm-ring-proof --target test_ringbuf_model test_ringbuf_process test_ringbuf_wrap test_ringbuf_boundary -j4
/tmp/shm-ring-proof/test_ringbuf_model
/tmp/shm-ring-proof/test_ringbuf_process
/tmp/shm-ring-proof/test_ringbuf_wrap
/tmp/shm-ring-proof/test_ringbuf_boundary
python3 test/ringbuf_mutation_check.py /tmp/shm-ring-proof-mutations
```

Release 构建验证 NDEBUG 路径，新测试的检查始终启用。Sanitizer 构建加 `-DSHM_IPC_SANITIZE=address,undefined`，运行加 `ASAN_OPTIONS=detect_leaks=1`。`_exit` 子进程不运行 LeakSanitizer 退出钩子，其资源释放通过内核及父进程观测验证。

## 证据限制

本轮没有证明弱内存模型下全部交错，没有 ARM 验证，没有新增生产 owner 的跨进程恢复测试，也没有外部专家审查。ASan/UBSan 不能证明无丢唤醒，有控制同步的过程不能作为弱内存序证明。下一步仍应建立与真实代码对应的并发模型。

## 本轮结果

- 两个新测试均通过 Debug、Release/NDEBUG 和 ASan/UBSan（开启 leak 检查）。
- 原有 Debug 回绕 25 项、边界 18 项全部通过；死亡测试的预期 assert 输出不代表失败。
- 四个 Release 变异实现均被检测并中止。
- 未发现需要修改生产 RingBuffer 的功能缺陷。

原始输出、变异反例和源文件指纹保存在 [evidence/ringbuf_2026-11-21](evidence/ringbuf_2026-11-21/)。生产实现的 SHA256 同时记录，方便第三方确认验证对象。

## 后续证据

有限 RC11 并发模型检查已完成，见 [并发内存序验证](ringbuf_rc11_validation_2026-11-21.md)。它补充上述证据，但不消除本文记录的跨平台和无限执行证明限制。
