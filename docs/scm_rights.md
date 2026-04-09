# SCM_RIGHTS：无亲缘关系进程间共享 fd 的原理

## 常见误解

"eventfd 能跨进程共享"——这个说法不准确。`eventfd()` 返回的 fd 编号只是当前进程文件描述符表里的一个索引（比如 fd=5），这个数字对另一个进程毫无意义。

能共享的关键不是 fd 编号，而是 fd 背后 **内核里的 `struct file` 对象**。

## 内核对象 vs 进程 fd 编号

```
进程 A 的 fd 表                 内核                    进程 B 的 fd 表
┌─────┐                                               ┌─────┐
│ fd=3│──────┐                                    ┌────│ fd=7│
│ fd=4│      │    ┌─────────────────────┐         │    │ fd=8│
│ fd=5│──┐   └───►│ struct file (eventfd)│◄────────┘    └─────┘
└─────┘  │        │  - 引用计数 = 2     │
         │        │  - 内部计数器       │
         │        └─────────────────────┘
         │        ┌─────────────────────┐
         └───────►│ struct file (memfd) │
                  └─────────────────────┘
```

`eventfd()` 在内核创建了一个匿名 file 对象（内含一个 uint64 计数器）。只要两个进程各持有一个指向同一个内核 file 的 fd，它们就共享同一个 eventfd。

## 两种共享方式

### fork：继承 fd 表

`fork()` 后子进程拷贝父进程的 fd 表，所有 fd 都指向相同的内核 file 对象。这是"有亲缘关系"的进程能共享的原因。

### SCM_RIGHTS：跨任意进程传递内核对象引用

没有 fork 关系的两个进程，需要一种方式让内核"把进程 A 的某个 fd 背后的 file 对象，在进程 B 的 fd 表里也安装一个入口"。这就是 `SCM_RIGHTS` 做的事。

## SendFd 到底做了什么

以本项目 `common.hpp` 中的 `SendFd` 为例：

```cpp
cmsg->cmsg_level = SOL_SOCKET;
cmsg->cmsg_type  = SCM_RIGHTS;                        // 告诉内核：这是 fd 传递
cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));       // 要传的 fd 编号

sendmsg(socket, &msg, 0);
```

调用 `sendmsg` 时，内核做的事 **不是** 简单地把 fd 数字发过去：

### sendmsg 内核路径

```
发送方 fd=5
  → 内核查发送方 fd 表
  → 得到 struct file *F
  → F->f_count++              // 增加引用计数，防止发送方关闭后对象被销毁
  → 把 F 挂到 socket 内核缓冲区
```

### recvmsg 内核路径

```
从 socket 缓冲区取出 struct file *F
  → 在接收方 fd 表找一个空位 → fd=7
  → fd_table[7] = F           // 安装到接收方 fd 表
  → 把 7 写入 CMSG_DATA 返回给用户态
```

**传递的不是数字，是内核对象的引用。** 接收方拿到的 fd 编号几乎肯定和发送方不同，但它们指向同一个内核对象。

## 本项目的握手流程

```
Client                                  Server
  │                                       │
  │  eventfd() → 内核创建 file_E          │
  │  memfd()   → 内核创建 file_M          │
  │                                       │
  │──SendFd(memfd)───SCM_RIGHTS─────────►│ recvmsg → 拿到 file_M 的新 fd
  │──SendFd(eventfd)─SCM_RIGHTS─────────►│ recvmsg → 拿到 file_E 的新 fd
  │                                       │
  │  此后：                               │
  │  client write(eventfd, 1)             │ server poll(eventfd) → POLLIN
  │  ↑ 操作的是同一个内核计数器            │ ↑ 同一个内核计数器
```

Client 创建 eventfd，通过 `SCM_RIGHTS` 发给 Server。之后双方各持有一个 fd 编号（数字不同），但指向内核里同一个 eventfd 计数器。Client 写 1，Server 的 poll 就会触发——因为它们操作的是同一个内核对象。

## 总结

| 概念 | 本质 |
|------|------|
| fd 编号 | 进程私有的索引号，跨进程无意义 |
| `struct file` | 内核对象，fd 编号只是指向它的指针 |
| `fork` 共享 | 子进程继承 fd 表 → 自动指向相同内核对象 |
| `SCM_RIGHTS` 共享 | 内核把发送方 fd 背后的 `struct file *` 安装到接收方 fd 表 |
| `SendFd` 做的事 | 请求内核执行上述 `SCM_RIGHTS` 操作 |
| eventfd 能跨进程 | 不是 eventfd 特殊，是任何 fd（eventfd / memfd / socket / pipe）都能通过 `SCM_RIGHTS` 共享 |
