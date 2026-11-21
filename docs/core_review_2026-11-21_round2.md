# 修复后核心代码复审（2026-11-21）

当前状态更新：用户依据交错 A/B 性能结果选择放弃 socketpair 通知方案，生产代码已切回非阻塞 eventfd。Debug 和 ASan/UBSan（含泄漏检查）各 14 个测试全部通过；工作区 build/ Release -O2 产物已重建。Release 构建出现测试故障注入文件 test_audit_regression.cpp 的两条 mismatched-new-delete 警告（该测试自定义 new/delete 并使用 malloc/free），核心实现未出现编译警告。下文 socketpair 修复描述保留为历史记录；第 5 项关于共享 fd 阻塞属性的条件性风险重新成为明确使用前提，要求可信对端遵守协议、保持 O_NONBLOCK。其余四项修复继续保留。通知测试已改为 eventfd 计数饱和、合并排空、本地唤醒、CLOEXEC 和端点属性/类型验证。


历史修复状态：下述五项问题曾全部修复；当前第 5 项按上文用户选择恢复为可信对端使用前提，以下保留修复前的审核记录。新增 `test/test_audit_round2_fixes.cpp` 覆盖一次性定时器重复排空、广播关闭当前连接及 Stop 清空连接表（含缓冲编码和直接编码）、慢握手隔离、同批连接跨重启、通知队列饱和及共享阻塞属性变更。

最终实现：Broadcast 在稳定的强引用快照上遍历；SendCmd 保活连接及 codec，拒绝编码重入，并在用户 codec 返回后复查关闭状态。所有 timerfd 使用 TFD_NONBLOCK。在途握手由受 max_clients 约束的独立任务执行，每条任务自创建起有 5 秒期限，Stop shutdown 并 join 所有任务；握手结果与整批收编同时校验运行代次。

通知改为 Unix datagram socketpair，收发显式使用 MSG_DONTWAIT，发送还使用 MSG_NOSIGNAL；通知排空次数受限，防止连续通知独占 loop。内部任务完成通知仍使用仅本进程持有的 eventfd。握手通知标签变为 kNotifySocket，与旧版共享 eventfd 协议不兼容，通信两端必须同步升级。通知计数表示本次消费的通知数量，不是消息数量。

修复后验证：13 个测试程序全部退出 0；8 个相关测试在 ASan/UBSan 和泄漏检查下全部退出 0；新增 round2_fixes 回归在 ThreadSanitizer 下退出 0，无 race 报告。构建日志没有新增编译警告。上述检查证明已覆盖的回归行为，不代替跨进程原子顺序的完整证明或长期性能基准。

审核对象：当前工作区，包含上一轮四项修复及现有未提交修改。此次没有修改核心代码。

结论：仍有四项 P1 问题，以及一项在不可信对端威胁模型下的 P1 问题，均已用独立探针复现。不能据现有测试通过认定已满足工业级交付标准。P1 表示应优先解决的内存安全、事件循环阻塞或连接生命周期问题；本报告没有证据将其升级为可远程执行代码等更严重结论。

## 1. P1：Broadcast 重入关闭连接导致释放后使用

位置：`include/shm_ipc/shm_server.hpp:247`、`:249`；`include/shm_ipc/shm_connection.hpp:146`。

Broadcast 直接引用 `conns_` 内的 shared_ptr 并调用 SendCmd，没有为当前连接持有独立强引用，也没有稳定的遍历快照。SendCmd 会调用用户提供的虚函数 Encode。该函数可以在同一事件循环线程调用 `server.CloseConn(conn_id)`，使当前节点从连接表删除。

复现条件：连接只有 server 连接表持有；从定时器回调调用 Broadcast；自定义 codec 的 Encode 先 CloseConn，再写入传入的编码缓冲。codec 对象另外持有强引用，排除了 codec 自身销毁的干扰。

ASan 检出：

```text
ERROR: AddressSanitizer: heap-use-after-free
WRITE of size 1
ReentrantCodec::Encode
shm::ShmConnection::SendCmd  shm_connection.hpp:146
shm::ShmServer::Broadcast   shm_server.hpp:249
```

被释放的是连接持有的 131072 字节 encode_buf_，释放链经过 ShmConnection 析构和 CloseConn。即使调用方额外保活连接以避免该缓冲释放，删除当前 unordered_map 节点后继续 range-for 仍涉及失效迭代器。仅增加当前连接的局部 shared_ptr 不足以修复整个问题。

建议：在进入任何用户 codec 调用前建立稳定的连接快照或其他不会因回调删节点而失效的遍历机制，保活当前连接，并在 Encode 返回后检查关闭状态，避免在已关闭连接上继续发布数据。回归覆盖关闭当前连接、关闭其他连接和 Stop 清空连接表。

## 2. P1：一次性定时器重复排空会永久阻塞 loop

位置：`include/shm_ipc/event_loop.hpp:218`、`:398`、`:428`。

AddOneShotTimer 包装回调在调用用户回调之前已读取 timerfd。公开的 DrainTimerfd 注释却要求在定时器回调中调用以清除事件；CreateTimerfd 仅使用 TFD_CLOEXEC，没有 TFD_NONBLOCK。

复现：

```cpp
loop.AddOneShotTimer(1, [&](int fd, short) {
    shm::EventLoop::DrainTimerfd(fd);
    loop.Stop();
}, &timer);
```

探针打印进入回调后不再返回，被 2 秒外部 timeout 终止，退出码 124。计数已被包装层清零，一次性定时器也没有后续到期事件，第二次 read 永久阻塞。跨线程 Stop 写入 wake_fd 无法解除这个 timerfd read。

建议：所有 loop 管理的 timerfd 使用 TFD_NONBLOCK，排空函数明确处理空计数的 EAGAIN；同时统一一次性与周期性定时器的消费契约。回归测试验证一次性回调再次 Drain、定时器计数被其他监听消耗及 Stop 的可达性。

## 3. P1：握手串行排队导致正常客户端超时

位置：`include/shm_ipc/shm_server.hpp:425`、`:437`、`:444`。

只有一个 HandshakeWorker，逐个执行最多 5 秒的阻塞握手。后台化保证 loop 不被握手直接阻塞，但没有隔离不同未完成握手的客户端。默认允许 16 个客户端，两个不发送任何握手数据的客户端就可以占据前两次处理时段，使其后正常客户端耗尽自己的 5 秒总期限。

复现：先连接两个原始 Unix socket 并保持打开、不发送 fd；200ms 后启动正常 ShmClient。loop 一直正常运行，输出：

```text
healthy client connected=0, error=3, elapsed_ms=5006
```

error=3 为 kTimeout。此时远未达到默认客户端数上限，正常客户端也没有协议错误。两条闲置连接已足以阻断新连接服务，队列越长越严重。

建议：使用有界的逐连接非阻塞握手状态机，让每条连接按自己的绝对期限推进和淘汰；若采用线程池，需要明确队列期限与准入策略，不能只增加线程数后继续无期限排队。回归验证多个慢连接存在时正常客户端仍能完成握手，并检查总 fd/映射资源上限。

## 4. P1：Stop/Start 后同批旧握手结果被收编到新服务

位置：`include/shm_ipc/shm_server.hpp:493`、`:495`、`:499`、`:515`。

OnHandshakeComplete 将 hs_done_ 交换到局部 ready 后，Stop 无法清理这个局部批次。若第一条连接的建连回调执行 Stop 后再 Start，started_ 会重新变为 true，后续旧连接仍通过检查并进入 FinishAccept。FinishAccept 此时保存的是新 run_generation_，因此上一轮新增的工厂调用代次检查无法识别这条旧连接。

复现：两条连接的握手均完成后再分发通知，模拟 loop 繁忙期间 eventfd 通知合并；第一条 connection 回调 Stop/Start。确定性调度探针通过 wrap accept4，在接受第二条连接后让 loop 的预先注册回调等待两条后台握手完成。输出：

```text
connection callbacks across restart: 2, remaining connections: 1
```

第二次建连回调来自重启前的剩余握手结果，新服务中仍有旧连接。Stop 后再次 Start 本身是已有支持的用法。

建议：在提取 ready 批次时保存其所属运行代次，整个批次只在该代次内处理；一旦 Stop/Start 改变代次，剩余结果全部静默销毁。最好让 HandshakeDone 显式携带代次，避免后续新增异步路径再次漏掉身份判断。回归必须包含同一批多条握手，不能只测一条连接的工厂回调。

## 5. 条件性 P1：对端握手后修改 eventfd 属性可阻塞本端通知

位置：`include/shm_ipc/common.hpp:702`、`:704`；`include/shm_ipc/ring_channel.hpp` 的 NotifyPeer 内 write。

威胁模型：对端是不可信本地进程，或对端错误修改共享 fd 的 file status flags。若产品明确只支持遵守协议且不修改共享 fd 属性的可信对端，这项应作为信任前提记录；不能把当前检查描述为能防御该行为。

SCM_RIGHTS 传递的 fd 共享同一个 open file description，O_NONBLOCK 也共享。握手时 ValidateNotifyEventfd 验证非阻塞属性，无法保证握手后仍保持该属性。对端可以操作自己持有的 eventfd：先写入 UINT64_MAX-1 填满计数，再 F_SETFL 清掉 O_NONBLOCK。本端下一次 NotifyPeer 的 write(1) 将阻塞。

复现：两端使用本库正常完成握手；服务端对自己的 NotifyReadFd 执行上述修改；客户端写入一字节并 NotifyPeer。探针停在 NotifyPeer，被 2 秒 timeout 终止，退出码 124。此前所有类型与 O_NONBLOCK 校验均已通过。

建议：若需要防御不可信对端，通知机制应避免让对端掌控本端 I/O 所用 open file description 的阻塞属性，例如具有独立两端文件描述的非阻塞 socketpair/管道方案。仅 dup、每次 write 前再 fcntl 或先 poll 都不能消除对端并发改属性的竞争。如果保留共享 eventfd，必须明确可信对端契约和准入边界。

## 验证记录与审核边界

本轮再次逐个运行 12 个现有测试程序，全部退出 0，包括上一轮新增的 core_fixes 测试。独立广播探针启用 ASan/UBSan，实际检出 heap-use-after-free；定时器和 eventfd 探针由外部 timeout 确认未返回；慢握手与批次重启探针取得上述输出。

临时探针源码：`/tmp/shm-review-round2.cpp`、`/tmp/shm-review-round2-batch.cpp`、`/tmp/shm-review-round2-eventfd.cpp`；ASan 日志：`/tmp/shm-review-round2-broadcast.log`；现有测试本轮日志：`/tmp/test_*.round2.log`。这些是审核复现材料，未加入 CMake 或修改核心实现。

现有测试通过与新探针失败并不矛盾：前者未覆盖本报告的触发条件。本轮不能证明跨进程原子顺序在所有目标架构上正确，也没有完成长时间压力、系统级资源耗尽或全部 syscall 的故障注入。上述限制不影响五条已复现问题的成立。
