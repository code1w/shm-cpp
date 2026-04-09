# eventfd 通知 vs Unix Socket 通知：机制与性能对比

## 两种唤醒方式概述

本项目中两个进程之间的数据传输和唤醒通知使用了不同组合：

| 方案 | 数据通道 | 唤醒通知 |
|------|---------|---------|
| Unix Socket | socket 内核缓冲区 | 数据本身即通知（`read` 返回即唤醒） |
| 共享内存 + eventfd | 共享 mmap 区域 | 共享 eventfd（`write(efd,1)` 唤醒对端 `poll`） |

## 内核路径对比

### Unix Socket 写数据 + 唤醒

```
write(socket_fd, data, len)
  → 进入内核 sys_write
  → socket 层: unix_stream_sendmsg()
    → skb_alloc()                    // 分配 socket 缓冲区
    → memcpy(skb, data, len)         // 用户空间 → 内核 skb
    → skb_queue_tail()               // 入队到接收方队列
    → wake_up_interruptible()        // 唤醒接收方
  → 返回用户空间

read(socket_fd, buf, len)            // 接收方
  → 进入内核 sys_read
  → socket 层: unix_stream_recvmsg()
    → skb_dequeue()                  // 从队列取出
    → memcpy(buf, skb, len)          // 内核 skb → 用户空间
    → skb_free()                     // 释放缓冲区
  → 返回用户空间
```

**每次传输 = 2 次 memcpy + skb 分配/释放 + 队列锁操作 + 唤醒**

数据和通知是耦合的：数据到达本身就会唤醒对端的 `poll`/`read`。

### 共享内存写数据 + eventfd 唤醒

```
// 写数据：纯用户态操作
memcpy(shared_mem + write_pos, data, len)  // 直接写共享内存
atomic_store(&write_pos, new_pos)          // 更新写指针
// 无系统调用，无内核参与

// 唤醒：轻量系统调用
write(eventfd, &1, 8)
  → 进入内核 sys_write
  → eventfd_write()
    → atomic_add(&counter, 1)        // 原子加计数器
    → wake_up_locked_poll()           // 唤醒等待者
  → 返回用户空间
```

**数据传输 = 0 次系统调用，1 次 memcpy（用户态到用户态）
唤醒通知 = 1 次轻量系统调用（仅操作一个 uint64 计数器）**

数据和通知是解耦的：数据走共享内存（无内核参与），通知走 eventfd（最轻量的系统调用之一）。

## 关键差异

```
                Unix Socket                    eventfd + 共享内存
                ===========                    ==================

  发送方        ┌─────────┐                    ┌─────────┐
  用户空间      │  data   │                    │  data   │
                └────┬────┘                    └────┬────┘
                     │ write()                      │ memcpy (用户态)
                     ▼                              ▼
  ─────────── 系统调用边界 ───────     ┌──────────────────────┐
                     │                 │  共享 mmap 区域       │
  内核空间      ┌────┴─────┐           │  (数据直达，无内核)   │
                │ skb 分配  │           └──────────────────────┘
                │ memcpy #1 │
                │ 队列入队  │                   │ write(efd, 1)
                │ 唤醒接收方│                   ▼
                └────┬─────┘           ┌─────────────────┐
                     │                 │ eventfd_write()  │
                ┌────┴─────┐           │ atomic_add       │
                │ 队列出队  │           │ wake_up          │
                │ memcpy #2 │           └─────────────────┘
                │ skb 释放  │
                └────┬─────┘
  ─────────── 系统调用边界 ───────
                     │ read()
                     ▼
  接收方        ┌─────────┐            接收方直接读共享内存
  用户空间      │  data   │            memcpy (用户态)
                └─────────┘
```

| 维度 | Unix Socket | eventfd + 共享内存 |
|------|------------|-------------------|
| 数据拷贝次数 | 2 次（用户→内核→用户） | 1 次（用户→共享内存，对端直接读） |
| 系统调用次数/轮 | 2 次（write + read） | 1 次通知（write eventfd）+ 0 次数据 |
| 内核内存分配 | 每次 skb_alloc/free | 无 |
| 通知机制 | 数据到达即通知（耦合） | 独立 eventfd（解耦） |
| 通知合并 | 不可合并，每条消息独立 | 天然合并，多次 write 可一次 read 全部 |
| 最大消息大小 | 受 socket 缓冲区限制 | 受共享内存大小限制（本项目 ~4MB） |
| 适用场景 | 通用 IPC，小消息 | 高频、低延迟、大吞吐 |

## 实测性能数据

在同一台机器（Linux 3.10, CentOS 7）上运行 ping-pong 回声基准测试：

### 小消息延迟（RTT 越低越好）

| msg_size | Socket RTT (ns) | SHM + eventfd RTT (ns) | 加速比 |
|---------:|-----------------:|-----------------------:|-------:|
| 64 | 11,714 | 1,886 | **6.2x** |
| 256 | 11,122 | 1,978 | **5.6x** |
| 1,024 | 9,898 | 2,633 | **3.8x** |
| 4,096 | 11,815 | 4,264 | **2.8x** |

### 大消息吞吐（MB/s 越高越好）

| msg_size | Socket (MB/s) | SHM + eventfd (MB/s) | 加速比 |
|---------:|--------------:|---------------------:|-------:|
| 64 | 10.42 | 64.72 | **6.2x** |
| 1,024 | 197.33 | 741.78 | **3.8x** |
| 65,536 | 3,910.97 | 4,618.70 | **1.2x** |
| 524,288 | 7,289.66 | 3,496.64 | **0.48x** |

### 分析

- **小消息（≤4KB）**：共享内存方案 RTT 低 3~6 倍。省掉的 2 次内核 memcpy 和 skb 分配/释放是主要收益。
- **中等消息（64KB）**：两者吞吐接近。数据拷贝开销开始主导，内核路径开销占比下降。
- **超大消息（512KB）**：Socket 反超。原因是 socket 的内核缓冲区实现针对大块连续传输做了优化（page splicing、zerocopy），而本项目的 ring buffer 在接近容量上限时频繁触发回绕逻辑。

## eventfd 为什么特别轻量

eventfd 是 Linux 内核中最简单的文件抽象之一，整个实现约 300 行代码：

```c
// 内核 fs/eventfd.c（简化）
struct eventfd_ctx {
    struct kref     kref;
    wait_queue_head_t wqh;
    __u64           count;     // 就这一个 uint64 计数器
    unsigned int    flags;
};
```

- `write(efd, &val, 8)`：原子加 `count += val`，若有等待者则 `wake_up`
- `read(efd, &val, 8)`：原子交换 `val = count; count = 0`，若 count==0 则阻塞或返回 EAGAIN
- 无缓冲区管理、无 skb、无队列、无锁竞争（单个 spinlock 保护 wait queue）

对比 Unix socket 的 `write`，省掉了：skb 分配、内存拷贝、队列操作、协议层处理。

## 通知合并特性

eventfd 的计数器语义提供了天然的通知合并：

```
写方连续 3 次：  write(efd, 1)  write(efd, 1)  write(efd, 1)
                         ↓
内核计数器：      count = 3
                         ↓
读方 1 次：      read(efd) → 返回 3, count 归零
```

如果写方高频写入、读方来不及处理，多次通知自动合并为一次唤醒。本项目的 `DrainNotify` 就是利用这个特性——一次 `read` 排空所有累积通知，然后处理环形缓冲区中所有待读消息。

Unix socket 无法合并：每次 `write` 的数据都会独立进入接收队列，接收方必须逐条 `read`。

## 总结

eventfd + 共享内存的组合将**数据传输**和**唤醒通知**解耦：

- 数据走共享内存 → 零系统调用、单次拷贝
- 唤醒走 eventfd → 最轻量的系统调用、天然合并

这使得它在高频小消息场景下比 Unix socket 快 3~6 倍。代价是需要 `SCM_RIGHTS` 握手交换 fd、手动管理环形缓冲区，以及在超大消息场景下可能不如内核优化过的 socket 路径。
