# 核心代码边界审核（2026-11-21）

修复状态：本报告下述四项问题已修复。新增 `test/test_audit_core_fixes.cpp` 覆盖 fd 0、重复验证定时器、工厂 Stop/Stop+Start、后台通知 EINTR、Unix 监听队列饱和及跨连接/握手的期限传递。12 个测试程序全部通过；5 个相关测试在 ASan/UBSan 与泄漏检测下全部通过。下文保留修复前发现与复现记录。

修复采用运行代次守卫、非阻塞且有期限的 connect（返回时恢复阻塞属性）、连接与握手共享期限、EINTR 通知重试以及 -1 定时器哨兵。通知发生非 EINTR/EAGAIN 的永久错误时，丢弃尚未收编的连接以避免无响应滞留。ETIMEDOUT 统一归类为 kTimeout。

审核对象为当前工作区（包含未提交修改），不是仅审核 HEAD。未修改业务代码。
重点覆盖 SPSC 环与镜像映射、帧长度与批量回滚、fd 交换、事件循环、连接生命周期和服务端握手。

结论：发现 4 个可复现问题，其中 3 个 P1、1 个 P2。当前不宜据现有测试通过就认定达到工业级交付标准。
P1 表示应优先修复的服务可用性或生命周期问题；P2 表示特定合法环境下的功能错误。

## 1. P1：codec 工厂回调停止服务后仍安装旧连接

位置：`include/shm_ipc/shm_server.hpp:503`，随后执行连接创建、`conns_` 插入及两个 fd 注册。

`OnHandshakeComplete` 在进入 `FinishAccept` 前检查 `started_`，但 `codec_factory_()` 是用户回调，可以在同一事件循环线程调用 `server.Stop()`。回调返回非空 codec 后，代码没有再次检查运行状态或服务代次，仍安装连接并调用 `OnConnection()`。

复现：工厂执行 `server.Stop(); return std::make_shared<comm::codec::Codec>();`。输出：

```text
connection callback after Stop in factory: 1, remaining connections: 1
```

影响：停止完成后仍产生建连事件并保留连接；后续 `Stop()` 因 `started_ == false` 直接返回。析构也走该分支，事件循环可能保留捕获已销毁 server 的回调，后续分发有悬垂对象访问风险。

建议：为 Start/Stop 引入运行代次，在调用工厂前保存代次，回调返回后同时检查 `started_` 和代次；失败则让临时 socket/channel 静默回收。检查其他可重入用户调用后的事务边界。回归测试应断言 Stop 后无建连事件、连接表为空、销毁 server 后 loop 仍可运行。

## 2. P1：connect 阶段没有期限，异步客户端可无限阻塞事件循环

位置：`include/shm_ipc/common.hpp:834`、`:848`；由同步和异步客户端 Connect 调用。

创建的是阻塞 Unix socket，直接调用 `connect()`。5 秒 `IoDeadline` 在后续 `CreateRingChannel()` 内才开始，不覆盖 connect 阶段。

复现：Unix listener 使用 backlog=1，先建立两个未 accept 的连接，再调用 `ConnectUnixSocket`。外部 6 秒超时终止该进程，输出：

```text
REPRO: ConnectUnixSocket still blocked after six seconds with full backlog
```

影响：繁忙或停止 accept 的服务端能让连接调用无限等待；ShmAsyncClient 在 loop 线程调用 Connect，会一起阻断定时器、收包和关闭处理。同步客户端尚未安装新 socket，Close 也不能关闭这个局部连接 fd。

建议：connect 使用非阻塞 socket，并按 AF_UNIX 的 EAGAIN 等语义实现有期限的重试/完成检查，不能直接照搬 TCP 的 EINPROGRESS 路径。connect 与 fd 交换共享总 deadline；验证队列饱和、服务端不 accept、EINTR 和失败后资源回收。

## 3. P1：后台握手完成通知遇到 EINTR 后丢失

位置：`include/shm_ipc/shm_server.hpp:467`。

握手结果加入 `hs_done_` 后只调用一次 eventfd `write()`，忽略返回值。若返回 EINTR，通知未写入，后台线程随后等待下个连接，loop 没有其他路径主动消费 `hs_done_`。

复现：链接 `--wrap=write`，仅在后台线程第一次 8 字节 write 注入 EINTR；客户端完成 fd 交换后保持 socket 打开。500ms loop 观察窗口内建连回调次数为 0，确认注入次数为 1。无后续成功握手时，该结果可以一直滞留。

影响：客户端握手成功，但服务端不收编、不处理数据，形成无响应连接。信号导致的罕见路径不应靠下个客户端连接来恢复。

建议：遇 EINTR 重试；EAGAIN 表示 eventfd 已可读，可以接受；其他错误必须有明确的恢复或停止路径。增加单次通知失败且没有下一条连接的故障注入测试。

## 4. P2：合法 fd 0 被当成“无验证定时器”

位置：`include/shm_ipc/shm_connection.hpp:249`、`:733`；`verify_timer_` 默认值和触发后的复位值为 0。

timerfd 可以返回 0，但 `if (verify_timer_)` 会把它认作未注册。验证成功时 `OnVerify(true)` 无法取消该定时器；重复启动也可能再次注册。

复现：创建 loop 和连接后关闭 stdin，让验证 timerfd 分配到 0，立即 `OnVerify(true)`，随后运行 loop。输出：

```text
verify callback after successful verify: 1
```

影响：已通过验证的连接仍触发超时动作，可能错误关闭连接；关闭 stdin 的守护进程、容器和测试环境均可能出现。

建议：未注册哨兵改为 -1，所有判断统一为 >=0，创建、取消及触发后的复位保持一致。测试 fd 0、重复启动、验证成功和连接关闭后的取消语义。

## 验证记录与限制

Debug 构建完成，11 个现有测试程序逐个运行并全部退出 0：四个 audit 测试、codec、eventloop、proto_codec、proto_wrap、ringbuf_boundary、ringbuf_wrap、shm_app。测试保留 assert，单程序外部超时为 45 秒。

另以 AddressSanitizer + UndefinedBehaviorSanitizer 构建并运行 ringbuf_boundary、audit_eof_reentry、audit_hardening、audit_second，启用泄漏检测，四个程序全部退出 0，未报告 sanitizer 错误。

独立复现文件位于本次会话的 `/tmp/shm-review-probes.cpp`、`/tmp/shm-review-notify.cpp`、`/tmp/shm-review-connect.cpp`；日志和构建位于 `/tmp/shm-review-build` 及 `/tmp/*.review.log`。这些临时文件不是仓库回归测试，修复时需要将相应断言纳入正式测试。

现有测试通过不能排除上述问题。本审核不宣称已证明全部跨进程内存模型、恶意对端持续修改共享内存时的行为或长期压力运行正确；动态内存检查也不能替代对跨进程原子顺序的分析。
