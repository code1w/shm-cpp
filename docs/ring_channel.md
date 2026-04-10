# RingChannel 技术文档

## 1. 架构总览

`RingChannel` 是 `shm_ipc` 库的核心组件，实现了基于共享内存的高性能双向进程间通信（IPC）通道。整体架构分为四个层次：

```
┌─────────────────────────────────────────────────────────┐
│              应用层 (server.cpp / client.cpp)             │
├─────────────────────────────────────────────────────────┤
│           编解码层 (codec.hpp)  SendPod / PodReader       │
├─────────────────────────────────────────────────────────┤
│         通道层 (ring_channel.hpp)  RingChannel<Cap>       │
├──────────────┬──────────────────────┬───────────────────┤
│  无锁环形缓冲 │   匿名共享内存        │  fd 传递与通知    │
│  ringbuf.hpp │  CreateMemfd/MmapRgn │  SCM_RIGHTS/efd   │
│              │  common.hpp          │  common.hpp       │
└──────────────┴──────────────────────┴───────────────────┘
```

### 设计理念

- **零系统调用数据路径**：建立通道后，读写操作仅访问 `mmap` 映射的用户态内存，无需 `read()`/`write()` 系统调用
- **无锁 SPSC**：单生产者-单消费者模型，仅依赖 C++11 原子操作和内存序保证正确性
- **纯 POD 共享内存布局**：无虚表、无堆指针、无 `std::atomic` 初始化问题——`memset` 清零即初始化
- **RAII 全生命周期管理**：`UniqueFd` 管理 fd，`MmapRegion` 管理 mmap 区域，异常安全

---

## 2. CreateMemfd — 匿名共享内存创建

### 2.1 原理

`memfd_create()` 是 Linux 3.17 引入的系统调用，创建一个**匿名的、基于 tmpfs 的内存文件**。它返回一个文件描述符，该 fd 不关联任何文件系统路径，但可以像普通文件一样进行 `ftruncate`、`mmap`、以及通过 `SCM_RIGHTS` 传递给其他进程。

与传统 POSIX 共享内存 (`shm_open`) 相比：

| 特性 | `shm_open` | `memfd_create` |
|------|-----------|----------------|
| 需要文件路径 | `/dev/shm/xxx` | 无（匿名） |
| 命名冲突风险 | 有 | 无 |
| 需要手动 `shm_unlink` | 是 | 否（fd 关闭即销毁） |
| 跨进程共享方式 | 通过路径名 | 通过 `SCM_RIGHTS` fd 传递 |
| 安全性 | 路径可被枚举 | 仅持有 fd 的进程可访问 |

### 2.2 实现

```cpp
// common.hpp:152-165
inline UniqueFd CreateMemfd(const char *name, std::size_t size)
{
    // 直接通过 syscall 调用，兼容不提供 memfd_create 包装的旧 glibc
    int fd = static_cast<int>(::syscall(__NR_memfd_create, name, 0u));
    if (fd < 0)
        throw std::runtime_error(...);

    // 设置共享内存大小
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0)
    {
        ::close(fd);
        throw std::runtime_error(...);
    }
    return UniqueFd{fd};
}
```

### 2.3 关键设计决策

1. **使用 `syscall(__NR_memfd_create)` 而非 `memfd_create()`**：直接使用系统调用号，避免依赖 glibc 2.27+ 的包装函数，提高在老版本系统上的可移植性。

2. **`name` 参数仅用于调试**：出现在 `/proc/<pid>/fd/` 的符号链接中（如 `memfd:client_to_server`），便于通过 `ls -la /proc/<pid>/fd/` 排查问题，不影响功能。

3. **flags = 0**：未使用 `MFD_CLOEXEC`（因为 fd 需要通过 `SCM_RIGHTS` 传递到其他进程），也未使用 `MFD_HUGETLB`（大页面）以保持通用性。

4. **异常安全**：`ftruncate` 失败时先 `close(fd)` 再抛异常，防止 fd 泄露。返回值包装为 `UniqueFd`，后续任何异常都会自动关闭 fd。

### 2.4 在 RingChannel 中的使用

每个 `RingChannel` 创建两个 memfd，分别承载两个方向的环形缓冲区：

```
进程 A (Client)                    进程 B (Server)
┌──────────────┐                  ┌──────────────┐
│ write_memfd_ │ ──memfd c2s───→ │ read_memfd_  │
│ (创建者,RW)  │   SCM_RIGHTS    │ (接收者,RW)  │
├──────────────┤                  ├──────────────┤
│ read_memfd_  │ ←──memfd s2c── │ write_memfd_ │
│ (接收者,RW)  │   SCM_RIGHTS    │ (创建者,RW)  │
└──────────────┘                  └──────────────┘
```


---

## 3. MmapRegion — 共享内存映射管理

### 3.1 原理

`mmap()` 将文件描述符（此处为 memfd）映射到进程的虚拟地址空间。当两个进程将同一个 memfd 通过 `MAP_SHARED` 映射后，它们直接共享同一片物理内存页——一个进程的写入对另一个进程**立即可见**（在正确的内存序约束下）。

```
虚拟地址空间 (进程A)         物理内存            虚拟地址空间 (进程B)
┌────────────────┐                            ┌────────────────┐
│ 0x7f...a000    │──┐                    ┌──→│ 0x7f...b000    │
│ write_region_  │  │   ┌────────────┐   │   │ read_region_   │
│                │  └──→│ memfd 物理页│───┘   │                │
└────────────────┘      │ (tmpfs)    │        └────────────────┘
                        └────────────┘
        进程A写入 ──→ 物理页更新 ──→ 进程B可见（acquire/release 屏障）
```

### 3.2 实现

```cpp
// common.hpp:75-122
class MmapRegion
{
public:
    MmapRegion() = default;

    // 核心构造：执行 mmap 映射
    MmapRegion(int fd, std::size_t size, int prot, int flags) : size_(size)
    {
        addr_ = ::mmap(nullptr, size, prot, flags, fd, 0);
        if (addr_ == MAP_FAILED)
            throw std::runtime_error(...);
    }

    // RAII 析构：自动 munmap
    ~MmapRegion()
    {
        if (addr_ != MAP_FAILED)
            ::munmap(addr_, size_);
    }

    // 移动语义：转移 mmap 所有权
    MmapRegion(MmapRegion &&o) noexcept
        : addr_(std::exchange(o.addr_, MAP_FAILED)), size_(o.size_) {}

    // 禁止复制（mmap 区域不能被复制）
    MmapRegion(const MmapRegion &)            = delete;
    MmapRegion &operator=(const MmapRegion &) = delete;

    void *Get() const noexcept { return addr_; }
    std::size_t Size() const noexcept { return size_; }

private:
    void *addr_       = MAP_FAILED;  // mmap 起始地址
    std::size_t size_ = 0;           // 映射长度
};
```

### 3.3 参数选择

在 `RingChannel` 中，映射参数如下：

```cpp
MmapRegion{fd, Ring::shm_size, PROT_READ | PROT_WRITE, MAP_SHARED};
```

| 参数 | 值 | 含义 |
|------|------|------|
| `addr` | `nullptr` | 由内核选择映射地址 |
| `size` | `Ring::shm_size` | `sizeof(RingHeader) + Capacity`（默认 8MB + 128B） |
| `prot` | `PROT_READ \| PROT_WRITE` | 读写权限（双方都需要读写：写方写数据，读方写 `read_pos`） |
| `flags` | `MAP_SHARED` | 修改对所有映射同一文件的进程可见 |
| `fd` | memfd | 匿名共享内存文件描述符 |
| `offset` | `0` | 从文件起始位置映射 |

### 3.4 为何双方都需要 PROT_WRITE

虽然是"读环"，但读方需要写入 `read_pos` 原子变量来推进读取位置。因此双方都必须以 `PROT_READ | PROT_WRITE` 映射。这是 SPSC 环形缓冲区的固有要求。

### 3.5 内存映射与 memfd 的生命周期

```
memfd_create() ──→ fd ──→ ftruncate(size) ──→ mmap(fd) ──→ 使用映射
                    │                            │
                    │  SCM_RIGHTS 传递给对端       │
                    ↓                            ↓
              对端 mmap(fd')                  close(fd)
                    │                         不影响映射！
                    ↓                            │
              共享物理页 ←─────────────────── munmap() 时释放
```

关键点：`close(fd)` 不会使已有的 mmap 映射失效。内核维护引用计数，只有所有映射都 `munmap` 且所有 fd 都关闭后，物理页才会被回收。

---

## 4. RingBuf — 无锁 SPSC 环形缓冲区

### 4.1 数据结构

`RingBuf<Capacity>` 是一个纯静态方法的工具类，操作位于共享内存中的 POD 数据结构：

```
共享内存布局 (shm_size = sizeof(RingHeader) + Capacity)
┌─────────────────────────────────────────────────────────────┐
│                    RingHeader (128 bytes)                     │
│  ┌──────────────────────────────┬──────────────────────────┐ │
│  │ write_pos (atomic<uint64>)   │ _pad1[56]  (cacheline)  │ │
│  │ 8 bytes                      │ 56 bytes                 │ │
│  ├──────────────────────────────┼──────────────────────────┤ │
│  │ read_pos  (atomic<uint64>)   │ _pad2[56]  (cacheline)  │ │
│  │ 8 bytes                      │ 56 bytes                 │ │
│  └──────────────────────────────┴──────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                 Data Region (Capacity bytes)                  │
│  ┌─────────┬────────┬──────────┬────┬─────────┬────────┐    │
│  │ MsgHdr  │payload │ pad to 8 │... │ MsgHdr  │payload │    │
│  │ len|seq │ (len B)│          │    │ len|seq │ (len B)│    │
│  └─────────┴────────┴──────────┴────┴─────────┴────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 缓存行填充（False Sharing 防护）

`RingHeader` 中 `write_pos` 和 `read_pos` 各自独占一个 64 字节缓存行：

```cpp
struct RingHeader
{
    std::atomic<uint64_t> write_pos;  // 写方独占
    char _pad1[56];                   // 填充到 64B 缓存行边界
    std::atomic<uint64_t> read_pos;   // 读方独占
    char _pad2[56];                   // 填充到 64B 缓存行边界
};
```

如果 `write_pos` 和 `read_pos` 位于同一缓存行，两个 CPU 核心分别修改它们时会触发**缓存行弹跳**（cache line bouncing），即 MESI 协议中的反复 Invalidate/Share 转换。填充后，两个变量位于不同缓存行，写方修改 `write_pos` 不会导致读方的 `read_pos` 缓存行失效，反之亦然。

### 4.3 单调递增位置与掩码寻址

```cpp
// write_pos, read_pos 为单调递增的字节偏移（永不回绕到 0）
// 物理偏移通过位掩码计算（Capacity 必须为 2 的幂）

static constexpr std::size_t Mask(uint64_t pos)
{
    return static_cast<std::size_t>(pos & (Capacity - 1));
}
```

**设计优势**：
- `write_pos - read_pos` 直接得到已用字节数，无需处理回绕的大小比较
- 利用 2 的幂容量，掩码运算（一条 AND 指令）替代取模运算
- `uint64_t` 单调递增，即使以 10 GB/s 速率写入也需要约 58 年才会溢出

### 4.4 消息帧格式

```
┌────────────┬────────────┬──────────────────┬──────────────┐
│  len (u32) │  seq (u32) │  payload (len B) │  pad to 8B   │
│  4 bytes   │  4 bytes   │  可变长度        │  0-7 bytes   │
└────────────┴────────────┴──────────────────┴──────────────┘
              MsgHeader (8B)

FrameSize(len) = (sizeof(MsgHeader) + len + 7) & ~7
```

- **8 字节对齐**：保证 `MsgHeader` 始终位于 8 字节对齐地址，避免未对齐访问的性能损失，同时保证哨兵帧至少可以写入 8 字节
- **`len = UINT32_MAX` 为哨兵**：标记该位置到环尾的空间废弃，读方遇到后跳转到偏移 0 继续读取

### 4.5 回绕哨兵机制

当写位置接近环形缓冲区物理末尾，剩余空间不足以容纳一个完整帧时：

```
写入前:
┌──────────────────────────────────────────────────┐
│ ... [msg3]  [msg4]  │←─ tail ─→│                 │
│                     ↑                             │
│               phys_w (write_pos & mask)           │
└──────────────────────────────────────────────────┘
                      ├─── tail < FrameSize(len) ──┤

写入后:
┌──────────────────────────────────────────────────┐
│ [MsgHdr][payload][pad] ... [msg4]  [SENTINEL]    │
│ ↑ 新消息从偏移0写入         ↑ len=UINT32_MAX     │
└──────────────────────────────────────────────────┘
write_pos += tail + FrameSize(len)  // 跳过 tail + 实际帧
```

### 4.6 内存序分析

这是整个系统正确性的关键。SPSC 模型下只有两个线程（进程），内存序要求最小化：

```
写方 (Producer)                    读方 (Consumer)
─────────────                     ─────────────
read  write_pos  [relaxed]  ①
read  read_pos   [acquire]  ②    ── 与 ⑥ 同步
                                  read  read_pos   [relaxed]  ④
写入 payload (memcpy)       ③    read  write_pos  [acquire]  ⑤ ── 与 ③ 同步
store write_pos  [release]  ③
                                  读取 payload (memcpy)       ⑤
                                  store read_pos   [release]  ⑥
```

| 操作 | 内存序 | 原因 |
|------|--------|------|
| 写方读 `write_pos` | `relaxed` | 仅写方修改此变量，无竞争 |
| 写方读 `read_pos` | `acquire` | 必须看到读方最新的 release store，确保已释放的空间可用 |
| 写方写 `write_pos` | `release` | payload 的 memcpy 必须在 write_pos 更新前对读方可见 |
| 读方读 `read_pos` | `relaxed` | 仅读方修改此变量，无竞争 |
| 读方读 `write_pos` | `acquire` | 必须看到写方 release store 之前的所有写入（payload） |
| 读方写 `read_pos` | `release` | 通知写方空间已释放 |

**在 x86-64 上的实际开销**：x86 的 TSO（Total Store Order）模型下，`acquire` 和 `release` 不需要额外的内存屏障指令，编译器只需防止指令重排即可。因此在 x86 上，这些原子操作的性能接近普通内存访问。

### 4.7 ForceWrite — 覆写最旧消息

```cpp
static void ForceWrite(void *shm, const void *data, uint32_t len, uint32_t seq)
```

当环已满时，`ForceWrite` 主动推进 `read_pos` 丢弃最旧的消息，直到腾出足够空间：

```
while ((w + cost) - r > Capacity)
{
    // 跳过哨兵或正常帧
    if (mh->len == kSentinel)
        r += Capacity - phys_r;
    else
        r += FrameSize(mh->len);
}
hdr->read_pos.store(r, std::memory_order_release);
```

**适用场景**：实时行情推送等"最新数据最重要"的场景。写方永不阻塞，但旧数据可能丢失。

### 4.8 PeekRead / CommitRead — 零拷贝读取

```cpp
// PeekRead：返回环内 payload 的原始指针，不推进 read_pos
static int PeekRead(void *shm, const void **data, uint32_t *len, uint32_t *seq);

// CommitRead：确认消费完毕，推进 read_pos
static void CommitRead(void *shm, uint32_t len);
```

```
TryRead（拷贝读）:    ring → memcpy → user_buf → 处理
PeekRead（零拷贝）:  ring → 直接处理（指针有效直到 CommitRead）
```

零拷贝路径减少一次 `memcpy`，在 bench 的回声测试中，服务端直接将 PeekRead 的指针传给 TryWrite，实现仅 1 次 memcpy 的 echo。


---

## 5. RingChannel — 双向通道封装

### 5.1 设计

`RingChannel<Capacity>` 将两个单向 `RingBuf` 组合为双向通道，并封装了 memfd 创建、mmap 映射、eventfd 通知以及 fd 交换握手的完整逻辑。

```
RingChannel 内部结构
┌──────────────────────────────────────────────────────────┐
│                     RingChannel                           │
│  ┌─────────────────────────┐  ┌────────────────────────┐ │
│  │ write_memfd_            │  │ read_memfd_            │ │
│  │ write_region_ ──→ Ring  │  │ read_region_ ──→ Ring  │ │
│  │ (我方创建, 写入方向)     │  │ (对端创建, 读取方向)   │ │
│  └─────────────────────────┘  └────────────────────────┘ │
│  ┌─────────────────────────┐  ┌────────────────────────┐ │
│  │ notify_write_efd_       │  │ notify_read_efd_       │ │
│  │ (写入以唤醒对端)        │  │ (对端写入以唤醒自身)   │ │
│  └─────────────────────────┘  └────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

### 5.2 四步握手协议

握手在 `Connect()` (客户端) 和 `Accept()` (服务端) 工厂方法中完成，通过 Unix domain socket 交换 4 个文件描述符：

```
      Client                         Server
        │                              │
        │  1. SendFd(memfd_c2s)       │
        │ ─────────────────────────→  │  RecvFdExpect(kMemfd)
        │                              │  mmap → read_region_
        │  2. SendFd(efd_client)      │
        │ ─────────────────────────→  │  RecvFdExpect(kEventfd)
        │                              │  → notify_write_efd_
        │                              │
        │                              │  创建 memfd_s2c, mmap
        │                              │  创建 efd_server
        │                              │
        │  3. RecvFdExpect(kMemfd)    │
        │ ←─────────────────────────  │  SendFd(memfd_s2c)
        │  mmap → read_region_        │
        │                              │
        │  4. RecvFdExpect(kEventfd)  │
        │ ←─────────────────────────  │  SendFd(efd_server)
        │  → notify_write_efd_        │
        │                              │
     握手完成，双向通道建立              握手完成
```

### 5.3 eventfd 通知机制

数据路径（环形缓冲区读写）不经过 socket，但需要一种机制让对端知道"有新数据到达"。这通过 `eventfd` 实现：

```cpp
// 写方：写入数据后通知对端
void NotifyPeer() const
{
    uint64_t v = 1;
    ::write(notify_write_efd_.Get(), &v, sizeof(v));
}

// 读方：通过 poll 监听 eventfd
int NotifyReadFd() const noexcept { return notify_read_efd_.Get(); }

// 读方回调中排空 eventfd
static uint64_t DrainNotify(int efd)
{
    uint64_t v = 0;
    ::read(efd, &v, sizeof(v));
    return v;
}
```

eventfd 的特性使其非常适合此场景：
- **轻量**：仅维护一个 64 位计数器，`write` 累加，`read` 归零并返回
- **可 poll/epoll/select**：可集成到事件循环（EventLoop 基于 poll）
- **`EFD_NONBLOCK`**：非阻塞模式，无数据时 `read` 返回 `EAGAIN`
- **`EFD_CLOEXEC`**：exec 时自动关闭，防止 fd 泄露

### 5.4 为何 eventfd 优于其他通知方案

| 方案 | 延迟 | fd 数量 | 实现复杂度 |
|------|------|---------|-----------|
| eventfd | ~1μs | 1 | 低 |
| pipe | ~1μs | 2 (读端+写端) | 中 |
| Unix socket | ~2μs | 1 | 高 |
| 信号 (SIGUSR) | 不确定 | 0 | 高，不可靠 |
| futex | ~0.5μs | 0 | 高，需共享内存 |
| 忙轮询 | ~0 | 0 | 最低，但 CPU 100% |

项目选择 eventfd：单 fd、可 poll、低延迟、实现简单。

---

## 6. SCM_RIGHTS — 文件描述符传递

### 6.1 原理

`SCM_RIGHTS` 是 Unix domain socket 的辅助消息（ancillary message）机制，允许一个进程将自己的文件描述符"传递"给另一个进程。内核会在目标进程中创建一个新的 fd 条目，指向同一个内核文件对象：

```
进程 A (发送方)                     进程 B (接收方)
fd 表:                              fd 表:
[3] ──→ struct file ←── [7]        内核创建新 fd 条目
        (memfd 内核对象)             指向同一 struct file
         ↓
     物理内存页（两个进程共享）
```

### 6.2 SendFd 实现

```cpp
// common.hpp:173-195
inline void SendFd(int socket, int fd, FdTag tag)
{
    // iovec: 承载类型标签（256字节缓冲区，前4字节为 FdTag）
    std::array<char, 256> dummy{};
    auto tag_val = static_cast<uint32_t>(tag);
    std::memcpy(dummy.data(), &tag_val, sizeof(tag_val));
    iovec io{dummy.data(), dummy.size()};

    // 控制消息缓冲区（对齐到 cmsghdr）
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};
    msghdr msg{};
    msg.msg_iov        = &io;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl.data();
    msg.msg_controllen = ctrl.size();

    // 填充 SCM_RIGHTS 控制消息
    auto *cmsg       = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (::sendmsg(socket, &msg, 0) < 0)
        throw std::runtime_error(...);
}
```

### 6.3 FdTag 类型安全校验

每次 fd 传递都附带一个 `FdTag` 标签，接收方通过 `RecvFdExpect` 校验：

```cpp
enum class FdTag : uint32_t
{
    kMemfd   = 0x4D454D46,  // "MEMF" 的 ASCII
    kEventfd = 0x45564644,  // "EVFD" 的 ASCII
};
```

这防止握手时序错乱——如果客户端和服务端的发送/接收顺序不一致，`RecvFdExpect` 会立即抛出异常，而不是静默地把 eventfd 当作 memfd 使用（这会导致难以排查的内存损坏）。

---

## 7. Codec — 类型安全的 POD 编解码

### 7.1 帧格式

```
┌──────────────┬──────────────────────┐
│ type_tag u32 │ payload (sizeof(T))  │
└──────────────┴──────────────────────┘
     4 bytes       sizeof(T) bytes
```

### 7.2 类型注册

通过 `SHM_IPC_REGISTER_POD` 宏将 POD 结构体与 4 字节标签绑定：

```cpp
struct Heartbeat { int32_t client_id; int32_t seq; int64_t timestamp; };
SHM_IPC_REGISTER_POD(Heartbeat, 0x48425454)  // "HBTT"
```

宏展开后特化 `TypeTag<Heartbeat>`，同时 `static_assert` 强制要求类型满足 `std::is_trivially_copyable_v`。未注册的类型在编译期即报错。

### 7.3 SendPod — 编码+写入+通知

```cpp
template <typename T, std::size_t Cap>
int SendPod(RingChannel<Cap> &ch, const T &obj, uint32_t seq)
{
    constexpr uint32_t frame_size = kTagSize + sizeof(T);
    char buf[frame_size];          // 栈上编码缓冲区
    Encode(obj, buf, frame_size);  // type_tag + memcpy
    int rc = ch.TryWrite(buf, frame_size, seq);
    if (rc == 0)
        ch.NotifyPeer();           // 写入成功，eventfd 通知对端
    return rc;
}
```

### 7.4 PodReader — 零拷贝流式读取器

`PodReader<T>` 直接操作 `RingChannel` 的 `PeekRead` 指针，实现零拷贝读取：

**快路径**（99%+ 场景）：
```
ring 消息 >= 帧大小 且 spill 无残留
→ 直接从 ring 指针 memcpy(sizeof(T)) 到 out
→ 1 次内存拷贝
```

**慢路径**（帧被拆分到多条 ring 消息）：
```
ring 消息 < 帧大小 或 spill 有残留
→ 按需拷贝字节到栈上定长 spill buffer
→ 攒够 kFrameSize 后解码
→ 无 vector、无 erase、无堆分配
```

```cpp
// 定长溢出缓冲区，完全在栈上
char spill_[kFrameSize]{};  // 分片拼接缓冲区
uint32_t used_ = 0;         // spill 中已有字节数
```

---

## 8. 完整数据流示例

以客户端发送 `ClientMsg` 到服务端为例，展示一条消息从发送到接收的完整路径：

```
Client 进程                                        Server 进程
─────────────                                     ─────────────

1. SendPod(channel, msg, seq)
   │
   ├→ Encode: [type_tag=0x434C4D47][ClientMsg bytes]
   │    栈上 buf[], 1次 memcpy
   │
   ├→ TryWrite(buf, frame_size, seq)
   │    │
   │    ├→ RingBuf::TryWrite(write_region_.Get(), ...)
   │    │    ├ 读 write_pos [relaxed]
   │    │    ├ 读 read_pos  [acquire]   ←── 与 Server 的 release 同步
   │    │    ├ 计算空间，写入 [MsgHdr][payload][pad]
   │    │    └ store write_pos [release] ──→ 发布新数据
   │    └→ return 0
   │
   └→ NotifyPeer()
        write(notify_write_efd_, 1)
        │
        │  ────── eventfd 内核通知 ──────
        │                                          │
        │                                    2. poll() 触发 POLLIN
        │                                       │
        │                                    3. DrainNotify(efd)
        │                                       read(efd) → 清除计数
        │                                       │
        │                                    4. PodReader::TryRecv(channel, &msg)
        │                                       │
        │                                       ├→ PeekRead(read_region_, ...)
        │                                       │    ├ 读 read_pos  [relaxed]
        │                                       │    ├ 读 write_pos [acquire]  ←── 看到 Client 的 release
        │                                       │    └ 返回 ring 内部指针
        │                                       │
        │                                       ├→ 快路径: memcpy(&out, ring_ptr+4, sizeof(T))
        │                                       │
        │                                       └→ CommitRead(len)
        │                                            store read_pos [release] ──→ 释放空间
```

---

## 9. 性能特征

### 9.1 延迟分析

| 阶段 | 操作 | 预期延迟 |
|------|------|----------|
| 编码 | memcpy × 2 (tag + payload) | < 100ns (L1 命中) |
| 写入 | atomic store + memcpy | < 200ns |
| 通知 | eventfd write | ~1μs (系统调用) |
| 唤醒 | poll 返回 | ~1-5μs |
| 读取 | atomic load + memcpy | < 200ns |
| **端到端 RTT** | | **~2-10μs（eventfd 通知）** |
| **忙轮询 RTT** | | **~200-500ns（无 eventfd）** |

### 9.2 吞吐量

以 bench_shm.cpp 的 echo 测试为参考，64B 消息 100K 轮次通常可达数 GB/s 吞吐量，大消息 (512KB) 受限于内存带宽。

### 9.3 相比传统 IPC 的优势

```
┌──────────────────────────────────────────────────────────────────┐
│ Unix socket:  用户态 → 系统调用 → 内核缓冲区 → 系统调用 → 用户态   │
│               send()     copy      copy       recv()             │
│               至少 2 次内核拷贝 + 2 次系统调用                      │
├──────────────────────────────────────────────────────────────────┤
│ RingChannel: 用户态 → mmap 共享页 → 用户态                        │
│              memcpy (1次)    atomic load                          │
│              0 次内核拷贝, 0 次数据路径系统调用                      │
└──────────────────────────────────────────────────────────────────┘
```

---

## 10. 源文件索引

| 文件 | 职责 |
|------|------|
| `include/shm_ipc/common.hpp` | `UniqueFd`, `MmapRegion`, `CreateMemfd`, `SendFd`/`RecvFd`, `CreateEventfd` |
| `include/shm_ipc/ringbuf.hpp` | `RingBuf<Cap>` — 无锁 SPSC 环形缓冲区核心算法 |
| `include/shm_ipc/ring_channel.hpp` | `RingChannel<Cap>` — 双向通道封装，fd 握手协议 |
| `include/shm_ipc/codec.hpp` | `Encode`/`Decode`, `SendPod`, `PodReader<T>` |
| `include/shm_ipc/messages.hpp` | `Heartbeat`, `ClientMsg` 应用层 POD 消息定义 |
| `include/shm_ipc/event_loop.hpp` | `EventLoop` — poll + timerfd 事件循环 |
| `include/shm_ipc/client_state.hpp` | `ClientState` — 连接状态聚合 |
| `src/server.cpp` | 多客户端服务端，EventLoop 驱动 |
| `src/client.cpp` | 客户端，定时发送 + eventfd 接收 |
| `bench/bench_shm.cpp` | fork + socketpair 回声延迟基准测试 |
