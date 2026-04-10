# shm_ipc 工程代码审查报告

| 项目 | 内容 |
|------|------|
| **文档编号** | SHM-IPC-CR-2026-001 |
| **日期** | 2026-04-10 |
| **审查范围** | shm_ipc 全量源码（include/shm_ipc/*.hpp, src/*.cpp, test/*.cpp, bench/*.cpp） |
| **审查标准** | 工业级 C++17，聚焦边界条件、崩溃风险、未定义行为、资源泄漏 |
| **严重程度定义** | CRITICAL=崩溃/数据损坏, HIGH=生产事故, MEDIUM=潜在隐患, LOW=代码质量 |

---

## 一、缺陷清单

### CRITICAL

#### CR-001 RecvFd 空指针解引用导致 SIGSEGV

- **文件**: `include/shm_ipc/common.hpp:218-220`
- **严重程度**: CRITICAL
- **现象**: `RecvFd()` 中 `CMSG_FIRSTHDR(&msg)` 在对端发送普通数据（无辅助消息）或连接异常断开时返回 `nullptr`。代码未做空指针检查，直接调用 `CMSG_DATA(cmsg)` 解引用，触发 SIGSEGV 崩溃。
- **触发条件**: 对端在 fd 握手中途崩溃；恶意客户端发送不携带 SCM_RIGHTS 的普通数据；网络模糊测试。
- **影响**: 服务端进程崩溃，所有已连接客户端断开。
- **修复方案**: 在 `CMSG_FIRSTHDR` 返回后立即校验指针非空及 `cmsg_type == SCM_RIGHTS`，不满足则抛出异常。
```cpp
auto *cmsg = CMSG_FIRSTHDR(&msg);
if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
    throw std::runtime_error("RecvFd: no SCM_RIGHTS in control message");
```

---

#### CR-002 ForceWrite 破坏 SPSC 不变量引发数据竞争

- **文件**: `include/shm_ipc/ringbuf.hpp:173-226`
- **严重程度**: CRITICAL
- **现象**: `ForceWrite` 由写方调用，但其内部通过 `hdr->read_pos.store(r, release)` 直接修改 `read_pos`。在 SPSC（单生产者-单消费者）模型中，`read_pos` 应仅由读方修改。当读方并发执行 `TryRead`（同样 store `read_pos`）时，两个进程同时写入同一原子变量，构成 C++ 标准定义的 data race（未定义行为）。
- **触发条件**: 写方调用 `ForceWrite` 的同时，读方正在执行 `TryRead` 或 `CommitRead`。
- **影响**: `read_pos` 被回退导致消息重复消费；读方读到被覆写的脏数据；极端情况下内存访问越界。
- **修复方案**: 将 `read_pos` 的直接 store 改为 `compare_exchange_strong` 循环。CAS 失败意味着读方已自行推进 `read_pos`（释放了更多空间），此时重新计算空间即可，无需强制覆写。
```cpp
if (new_r != r)
{
    if (!hdr->read_pos.compare_exchange_strong(
            r, new_r, std::memory_order_release, std::memory_order_acquire))
        goto retry;  // 读方已推进，重新计算
}
```

---

#### CR-003 PodReader spill buffer 越界写导致栈破坏

- **文件**: `include/shm_ipc/codec.hpp:220-228`（快路径）及第 251、258 行（慢路径）
- **严重程度**: CRITICAL
- **现象**: `PodReader<T>` 的 `spill_` 缓冲区大小为 `kFrameSize`（即 `kTagSize + sizeof(T)`）。在快路径中，当一条 ring 消息的 `len` 超过 `2 * kFrameSize` 时，`leftover = len - kFrameSize` 超过 `spill_` 容量，`memcpy(spill_, src + kFrameSize, leftover)` 越界写入栈内存。慢路径的两处 leftover 写入存在相同问题。
- **触发条件**: 应用层通过 ring 发送的单条消息大于 `2 * (kTagSize + sizeof(T))` 字节。例如 `sizeof(T)=16` 时 `kFrameSize=20`，一条 ring 消息超过 40 字节即可触发。
- **影响**: 栈内存被覆写，可能导致返回地址篡改（RCE）或不可预测的崩溃。
- **修复方案**: 在所有三处 leftover 写入点添加上界限制，确保不超过 `kFrameSize`：
```cpp
uint32_t to_spill = (leftover <= kFrameSize) ? leftover : kFrameSize;
std::memcpy(spill_, src + kFrameSize, to_spill);
used_ = to_spill;
```

---

#### CR-004 server.cpp 同轮 poll 多回调触发导致 use-after-free

- **文件**: `src/server.cpp:166-232`
- **严重程度**: CRITICAL
- **现象**: 每个客户端注册了三个 fd 到 EventLoop（socket fd、eventfd、timer fd），回调中捕获了裸指针 `csp`（指向 `clients[client_fd]` 中 `unique_ptr` 管理的 `ClientState`）。当客户端断连时，socket fd 和 eventfd 可能在同一轮 `poll()` 中同时就绪。第一个回调执行 `RemoveClient()` → `clients.erase(client_fd)` 后，`csp` 成为悬空指针。第二个回调继续通过 `csp` 访问已释放内存。
- **触发条件**: 客户端断连时 socket fd（POLLHUP）和 eventfd（POLLIN，因为对端在断连前刚写入通知）在同一次 poll 中同时返回事件。
- **影响**: use-after-free，可能导致崩溃或内存损坏。
- **修复方案**: 所有回调不再捕获裸 `csp` 指针，改为捕获 `&clients` 和 `client_fd`，在回调入口通过 `clients.find(client_fd)` 查找，不存在则直接返回：
```cpp
loop.AddFd(csp->notify_efd, [&clients, client_fd](int efd, short) {
    Channel::DrainNotify(efd);
    auto it = clients.find(client_fd);
    if (it == clients.end())
        return;  // 已被其他回调移除
    ClientState *csp = it->second.get();
    // ...
});
```

---

### HIGH

#### CR-005 EventLoop 回调中 AddFd 触发 vector 重分配导致迭代器失效

- **文件**: `include/shm_ipc/event_loop.hpp:106-136`
- **严重程度**: HIGH
- **现象**: `EventLoop::Run()` 遍历 `pfds` 并通过索引 `entries_[i].cb(...)` 调用回调。回调内部（如 server.cpp 的 accept 回调）调用 `AddFd()` 会 `push_back` 到 `entries_`。当 `entries_` 的 `capacity()` 不足时触发重分配，`entries_` 的内存地址改变，后续循环中 `entries_[i]` 的访问指向已释放内存。
- **触发条件**: `entries_` 容量不足以容纳新 fd 时，在回调中调用 `AddFd`。
- **影响**: 访问已释放内存，未定义行为。
- **修复方案**: 仿照已有的 `pending_removals_` 延迟删除机制，增加 `pending_additions_` 延迟添加队列。在回调分发期间设置 `dispatching_` 标志，`AddFd` 检测到此标志时将新条目加入 `pending_additions_`，分发结束后统一追加到 `entries_`。

---

#### CR-006 信号处理器写入非原子 bool 为未定义行为

- **文件**: `src/server.cpp:44-48`, `src/client.cpp:34-38`, `include/shm_ipc/event_loop.hpp:178`
- **严重程度**: HIGH
- **现象**: `SignalHandler` 通过 `gLoop->Stop()` 写入 `EventLoop::running_`（普通 `bool`）。C++17 标准 [intro.execution]/6 规定：在信号处理器中访问非 `volatile sig_atomic_t` 或非锁无关原子类型的变量是未定义行为。编译器可能将 `running_` 提升到寄存器缓存，信号处理器的写入被忽略，导致 `Run()` 循环永不退出。
- **触发条件**: 在信号处理器中调用 `Stop()`（Ctrl+C 或 kill 信号）。
- **影响**: 程序可能无法正常退出。
- **修复方案**: 将 `running_` 类型改为 `std::atomic<bool>`，所有访问使用 `relaxed` 序（信号处理器场景下 relaxed 即足够保证可见性）。

---

#### CR-007 NotifyPeer 忽略 write 返回值

- **文件**: `include/shm_ipc/ring_channel.hpp:198-202`
- **严重程度**: HIGH
- **现象**: `write(notify_write_efd_, &v, sizeof(v))` 的返回值被完全忽略。可能的失败场景：`EINTR`（信号中断）导致通知丢失；`EAGAIN`（eventfd 计数器溢出至 `UINT64_MAX - 1`）；`EBADF`（fd 意外关闭）。
- **触发条件**: 高频写入场景下信号中断。
- **影响**: 通知丢失导致读方无法及时唤醒，消息延迟增大。
- **修复方案**: 添加 `EINTR` 重试循环：
```cpp
ssize_t r;
do {
    r = ::write(notify_write_efd_.Get(), &v, sizeof(v));
} while (r < 0 && errno == EINTR);
```

---

#### CR-008 fd 握手阻塞无超时可拖垮服务端

- **文件**: `include/shm_ipc/ring_channel.hpp:54-78, 92-117`
- **严重程度**: HIGH
- **现象**: `Connect()` 和 `Accept()` 中的 `SendFd` / `RecvFdExpect` 使用阻塞式 `sendmsg` / `recvmsg`，无超时保护。恶意客户端建立 TCP 连接后不发送任何 fd，`Accept` 中的 `recvmsg` 将永久阻塞。由于握手在 EventLoop 的 accept 回调中同步执行，整个事件循环被卡死，所有已连接客户端的心跳和消息处理全部停止。
- **触发条件**: 恶意半连接；客户端在握手第一步后崩溃。
- **影响**: 服务端 EventLoop 死锁，等同于拒绝服务。
- **修复方案**: 在 `Connect` 和 `Accept` 入口调用 `setsockopt(SO_RCVTIMEO / SO_SNDTIMEO)` 设置 5 秒超时。超时后 `recvmsg` 返回 `EAGAIN`/`EWOULDBLOCK`，后续代码检查到 < 0 后抛出异常，连接被清理。

---

#### CR-009 MmapRegion 移动赋值未重置源对象 size_

- **文件**: `include/shm_ipc/common.hpp:96-102`
- **严重程度**: HIGH
- **现象**: 移动赋值运算符中 `size_ = o.size_` 但未将 `o.size_` 置零。移动后源对象的 `addr_` 为 `MAP_FAILED`（析构安全），但 `Size()` 返回错误的非零值。如果移动后的源对象被意外使用（如作为参数传递给需要检查 `Size()` 的函数），会产生逻辑错误。
- **触发条件**: 移动赋值后对源对象调用 `Size()`。
- **影响**: 逻辑错误，可能导致后续代码基于错误的大小值做出错误决策。
- **修复方案**: 使用 `std::exchange`：
```cpp
size_ = std::exchange(o.size_, 0);
```

---

### MEDIUM

#### CR-010 sendmsg 部分发送未处理

- **文件**: `include/shm_ipc/common.hpp:193`
- **严重程度**: MEDIUM
- **现象**: `sendmsg()` 返回值仅检查 `< 0`，未验证实际发送字节数等于预期。Unix domain socket 上通常不会部分发送，但 POSIX 标准未保证。
- **触发条件**: 理论上在极端内存压力下可能发生。
- **影响**: 对端收到不完整数据，后续 `RecvFd` 解析错乱。
- **修复方案**: 检查返回值等于 `io.iov_len`。

---

#### CR-011 recvmsg 未检查连接关闭（返回 0）

- **文件**: `include/shm_ipc/common.hpp:218`
- **严重程度**: MEDIUM
- **现象**: `recvmsg()` 返回 0 表示对端已关闭连接，但原代码仅检查 `< 0`。返回 0 时 `CMSG_FIRSTHDR` 可能返回 nullptr（已被 CR-001 修复捕获），但语义上应区分"连接关闭"和"控制消息缺失"两种错误。
- **触发条件**: 对端在 fd 传递过程中正常关闭连接。
- **影响**: 错误信息不够精确，不利于排查问题。
- **修复方案**: 增加 `received == 0` 的显式检查并抛出明确异常。

---

#### CR-012 RingBuf 对损坏 shm 数据无防御

- **文件**: `include/shm_ipc/ringbuf.hpp:238-273, 285-316`
- **严重程度**: MEDIUM
- **现象**: `TryRead` 和 `PeekRead` 直接信任 `mh->len` 的值。如果共享内存被意外损坏（硬件错误、程序 bug 导致越界写），`payload_len` 可能是任意值，`memcpy(data, ..., payload_len)` 可能读取远超环形缓冲区边界的内存。
- **触发条件**: 共享内存数据损坏。
- **影响**: 越界读可能导致 SIGSEGV 或敏感数据泄漏。
- **修复方案**: 在读取 `payload_len` 后添加上界检查：
```cpp
if (payload_len > max_msg_size)
    return -1;
```

---

### LOW

#### CR-013 UniqueFd 缺少 [[nodiscard]] 属性

- **文件**: `include/shm_ipc/common.hpp:33`
- **严重程度**: LOW
- **现象**: `CreateMemfd()` 等工厂函数返回 `UniqueFd`，如果调用方忽略返回值（如忘记赋值），fd 会被构造后立即析构关闭，静默泄漏资源。编译器不会产生警告。
- **修复方案**: 在 `UniqueFd` 类声明上添加 `[[nodiscard]]` 属性。

---

## 二、修复验证记录

| 验证项 | 结果 |
|--------|------|
| `cmake --build build` 全量编译 | PASS — 零错误、零警告 |
| `./build/test_codec` 逐字节分片解码测试 | PASS — 4108 字节帧逐字节写入后正确解码 |
| `./build/bench_shm` 回声延迟基准测试 | PASS — 64B RTT 775ns, 64KB 吞吐 7.5GB/s，性能无回退 |

---

## 三、修复文件清单

| 文件 | 涉及缺陷编号 |
|------|-------------|
| `include/shm_ipc/common.hpp` | CR-001, CR-009, CR-010, CR-011, CR-013 |
| `include/shm_ipc/ringbuf.hpp` | CR-002, CR-012 |
| `include/shm_ipc/codec.hpp` | CR-003 |
| `src/server.cpp` | CR-004 |
| `include/shm_ipc/event_loop.hpp` | CR-005, CR-006 |
| `include/shm_ipc/ring_channel.hpp` | CR-007, CR-008 |
