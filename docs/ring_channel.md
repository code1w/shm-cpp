# RingChannel 技术文档

## 1. 架构总览

`RingChannel` 是 `shm_ipc` 库的核心组件，实现了基于共享内存的高性能双向进程间通信（IPC）通道。整体架构分为四个层次：

```
┌─────────────────────────────────────────────────────────┐
│     应用层 (server.cpp / client.cpp / ShmServer 等)       │
├─────────────────────────────────────────────────────────┤
│   编解码层 (codec.hpp / frame_reader.hpp)                 │
│   Send / SendFrame / SendFrameBatch / FrameReader         │
├─────────────────────────────────────────────────────────┤
│         通道层 (ring_channel.hpp)  RingChannel<Cap>       │
├──────────────┬──────────────────────┬───────────────────┤
│  无锁字节环   │   匿名共享内存        │  fd 传递与通知    │
│  ringbuf.hpp │  CreateMemfd/MmapRgn │  SCM_RIGHTS/efd   │
│              │  common.hpp          │  common.hpp       │
└──────────────┴──────────────────────┴───────────────────┘
```

### 设计理念

- **零系统调用数据路径**：建立通道后，读写操作仅访问 `mmap` 映射的用户态内存；仅当读方已睡眠时，写方才发一次 `write(eventfd)` 唤醒（见 4.8）
- **无锁 SPSC**：单生产者-单消费者模型，仅依赖 C++11 原子操作和内存序保证正确性
- **纯字节流环**：环层没有帧头、哨兵、对齐填充；消息边界由 codec 层的 `[MsgHeader 8B][payload]` 管理
- **纯 POD 共享内存布局**：无虚表、无堆指针；`RingHeader` 在 mmap 内存上原地构造（`Init`），使其中的 `std::atomic` 拥有合法的对象生命周期
- **RAII 全生命周期管理**：`UniqueFd` 管理 fd，`MmapRegion` 管理 mmap 区域
- **公共 API 不抛显式异常**：失败返回 `shm::ErrorCode`（`error.hpp`），系统调用的 errno 通过可选的 `int *sys_errno` 出参返回

---

## 2. CreateMemfd — 匿名共享内存创建

### 2.1 原理

`memfd_create()` 是 Linux 3.17 引入的系统调用，创建一个**匿名的、基于 tmpfs 的内存文件**。它返回一个文件描述符，该 fd 不关联任何文件系统路径，但可以像普通文件一样进行 `ftruncate`、`mmap`、以及通过 `SCM_RIGHTS` 传递给其他进程。

与传统 POSIX 共享内存 (`shm_open`) 相比：

| 特性                    | `shm_open`     | `memfd_create`            |
| ----------------------- | ---------------- | --------------------------- |
| 需要文件路径            | `/dev/shm/xxx` | 无（匿名）                  |
| 命名冲突风险            | 有               | 无                          |
| 需要手动 `shm_unlink` | 是               | 否（fd 关闭即销毁）         |
| 跨进程共享方式          | 通过路径名       | 通过 `SCM_RIGHTS` fd 传递 |
| 安全性                  | 路径可被枚举     | 仅持有 fd 的进程可访问      |

### 2.2 实现

```cpp
// common.hpp
[[nodiscard]]
inline ErrorCode CreateMemfd(const char *name, std::size_t size,
                             UniqueFd *out, int *sys_errno = nullptr) noexcept
{
    // 直接通过 syscall 调用，兼容不提供 memfd_create 包装的旧 glibc
    int fd = syscall(__NR_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 && errno == EINVAL)   // 旧内核不认识 MFD_ALLOW_SEALING
        fd = syscall(__NR_memfd_create, name, MFD_CLOEXEC);
    if (fd < 0)
        return ErrnoToErrorCode(errno);          // errno 经 sys_errno 返回

    if (::ftruncate(fd, size) < 0)
    {
        ::close(fd);                             // 失败路径不泄露 fd
        return ErrnoToErrorCode(errno);
    }
    // 封印尺寸：之后任何持有者都无法 ftruncate 改变大小
    (void)::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
    *out = UniqueFd{fd};
    return ErrorCode::kOk;
}
```

### 2.3 关键设计决策

1. **使用 `syscall(__NR_memfd_create)` 而非 `memfd_create()`**：直接使用系统调用号，避免依赖 glibc 2.27+ 的包装函数，提高在老版本系统上的可移植性。
2. **`name` 参数仅用于调试**：出现在 `/proc/<pid>/fd/` 的符号链接中（如 `memfd:client_to_server`），便于通过 `ls -la /proc/<pid>/fd/` 排查问题，不影响功能。
3. **`MFD_CLOEXEC | MFD_ALLOW_SEALING`**：`MFD_CLOEXEC` 只影响本进程 `exec` 时是否继承该 fd，不影响 `SCM_RIGHTS` 传递给其他进程，因此可以安全开启，防止 fd 泄露给子进程。未使用 `MFD_HUGETLB` 以保持通用性。
4. **创建即封印（seal）**：`F_SEAL_SHRINK | F_SEAL_GROW` 使文件大小不可再变。否则恶意对端在握手后 `ftruncate` 缩小文件，接收方映射区域会因访问已被截断的页而触发 `SIGBUS`。seal 不影响已建立的可写映射。不支持 seal 的内核静默跳过，仅失去这层硬化。
5. **错误码而非异常**：`ftruncate` 失败时先 `close(fd)` 再返回错误码；成功时才把 fd 交给出参 `UniqueFd`。

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

`MmapRegion` 是默认可构造的 RAII 类，映射通过 `Map()` / `MapMirrored()` 建立，二者都返回 `ErrorCode`：

```cpp
// common.hpp
class MmapRegion
{
public:
    MmapRegion() = default;
    ~MmapRegion();                                   // 自动 munmap
    MmapRegion(MmapRegion &&) noexcept;              // 移动语义
    MmapRegion &operator=(MmapRegion &&) noexcept;
    MmapRegion(const MmapRegion &)            = delete;
    MmapRegion &operator=(const MmapRegion &) = delete;

    // 普通映射：整个文件一次 mmap。失败时保留原映射不变
    ErrorCode Map(int fd, std::size_t size, int prot, int flags,
                  int *sys_errno = nullptr) noexcept;

    // 数据区镜像映射（见 3.6）
    ErrorCode MapMirrored(int fd, std::size_t file_size,
                          std::size_t data_offset, std::size_t capacity,
                          int prot, int flags, int *sys_errno = nullptr) noexcept;

    void *Get() const noexcept;
    std::size_t Size() const noexcept;
    bool Mirrored() const noexcept;                  // 是否为镜像映射
    explicit operator bool() const noexcept;         // 是否已建立映射

private:
    void *addr_ = MAP_FAILED;     // 镜像时为 VA 预留起点
    std::size_t size_ = 0;        // munmap 长度（镜像时为 VA 预留长度）
    bool mirrored_ = false;
    std::size_t data_offset_ = 0;
};
```

### 3.3 参数选择

`RingChannel` 优先建立镜像映射，失败时退化为普通映射：

```cpp
region.MapMirrored(fd, Ring::shm_size, kRingDataOffset, Ring::capacity,
                   PROT_READ | PROT_WRITE, MAP_SHARED);
// 失败 → region.Map(fd, Ring::shm_size, PROT_READ | PROT_WRITE, MAP_SHARED);
```

| 参数            | 值                         | 含义                                                        |
| --------------- | -------------------------- | ----------------------------------------------------------- |
| `size`        | `Ring::shm_size`         | `kRingDataOffset + Capacity`（默认 4096 + 8MB）           |
| `data_offset` | `kRingDataOffset = 4096` | 数据区在文件中的页对齐偏移；之前是 `RingHeader` + 填充页 |
| `prot`        | `PROT_READ \| PROT_WRITE` | 读写权限（双方都需要读写：写方写数据，读方写 `read_pos`） |
| `flags`       | `MAP_SHARED`             | 修改对所有映射同一文件的进程可见                            |
| `fd`          | memfd                      | 匿名共享内存文件描述符                                      |

### 3.4 为何双方都需要 PROT_WRITE

虽然是"读环"，但读方需要写入 `read_pos` 和 `reader_wait` 原子变量。因此双方都必须以 `PROT_READ | PROT_WRITE` 映射。这是 SPSC 环形缓冲区的固有要求。

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

### 3.6 数据区镜像映射（MapMirrored）

字节环的数据在环尾会回绕。普通映射下，跨环尾的读写必须拆成两段 `memcpy`，`Peek` 也要返回两段指针。镜像映射让同一块物理数据区在虚拟地址上**连续映射两圈**，消除这些特殊处理：

```
VA 预留: file_size + capacity，先 PROT_NONE 占位，再用 MAP_FIXED 覆盖

base                        base+4096          base+4096+Cap      base+4096+2·Cap
 │◄── header/填充页 ──►│◄──── 数据区（第一圈）────►│◄── 数据区（第二圈）──►│
 │ 文件[0, 4096)        │ 文件[4096, 4096+Cap)      │ 文件[4096, 4096+Cap)   │
                                    ▲                         ▲
                                    └──── 同一批物理页 ────────┘
```

任何起点在 `[0, Capacity)`、长度 ≤ `Capacity` 的窗口，在 VA 上都是连续的：

- 写入/读取跨环尾：**单次 `memcpy`**
- `Peek`：永远只返回一段指针
- `Reserve`：不再因为跨环尾而失败

前提是 `kRingDataOffset` 页对齐（`MapMirrored` 会校验）。镜像只影响本进程的 VA 布局，两端可以一边镜像、一边普通映射，互相兼容；镜像映射失败时自动退化为普通映射，两段式路径保持可用。

---

## 4. RingBuf — 无锁 SPSC 环形缓冲区

### 4.1 数据结构

`RingBuf<Capacity>` 是一个纯静态方法的工具类，操作位于共享内存中的 POD 数据结构：

```
共享内存布局 (shm_size = kRingDataOffset + Capacity = 4096 + Capacity)
┌─────────────────────────────────────────────────────────────┐
│                    RingHeader (192 bytes, 3 个缓存行)         │
│  ┌──────────────────────────────┬──────────────────────────┐ │
│  │ write_pos (atomic<uint64>)   │ pad1[56]                 │ │
│  ├──────────────────────────────┼──────────────────────────┤ │
│  │ read_pos  (atomic<uint64>)   │ pad2[56]                 │ │
│  ├──────────────────────────────┼──────────────────────────┤ │
│  │ reader_wait (atomic<uint32>) │ pad3[60]                 │ │
│  └──────────────────────────────┴──────────────────────────┘ │
│                    填充至 4096 字节（页对齐）                  │
├─────────────────────────────────────────────────────────────┤
│                 Data Region (Capacity bytes)                  │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ [byte][byte][byte] ... 纯字节流，无帧头/哨兵/对齐填充   │    │
│  └──────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 缓存行填充（False Sharing 防护）

`RingHeader` 中 `write_pos`、`read_pos`、`reader_wait` 各自独占一个 64 字节缓存行：

```cpp
struct alignas(64) RingHeader
{
    std::atomic<uint64_t> write_pos;    // 写方独占
    char pad1[64 - sizeof(std::atomic<uint64_t>)];
    std::atomic<uint64_t> read_pos;     // 读方独占
    char pad2[64 - sizeof(std::atomic<uint64_t>)];
    std::atomic<uint32_t> reader_wait;  // 读方置位、写方清零
    char pad3[64 - sizeof(std::atomic<uint32_t>)];
};
static_assert(sizeof(RingHeader) == 192);
static_assert(alignof(RingHeader) == 64);
```

如果这些变量位于同一缓存行，两个 CPU 核心分别修改它们时会触发**缓存行弹跳**（cache line bouncing），即 MESI 协议中的反复 Invalidate/Share 转换。填充后，写方修改 `write_pos` 不会导致读方的 `read_pos` 缓存行失效，反之亦然。`reader_wait` 单独一行，避免读方睡眠/唤醒标志的翻转干扰这两个热点变量。

### 4.3 单调递增位置与掩码寻址

```cpp
// write_pos, read_pos 为单调递增的字节偏移（永不回绕到 0）
// 物理偏移通过位掩码计算（Capacity 必须为 2 的幂）

template <std::size_t Capacity>
constexpr std::size_t RingPhys(uint64_t pos)
{
    return static_cast<std::size_t>(pos & (Capacity - 1));
}
```

**设计优势**：

- `write_pos - read_pos` 直接得到已用字节数，无需处理回绕的大小比较
- 利用 2 的幂容量，掩码运算（一条 AND 指令）替代取模运算
- `uint64_t` 单调递增，即使以 10 GB/s 速率写入也需要约 58 年才会溢出

容量约束（`static_assert`）：2 的幂、不小于 1024、不超过 `UINT32_MAX`（长度字段为 `uint32_t`）。单次写入上限 `max_write_size = Capacity / 2`。

### 4.4 纯字节流：无帧头、无哨兵

环层只搬运字节，不理解消息边界：

```
写入 len 字节: 要求 (write_pos + len) - read_pos <= Capacity，否则返回 -1
读取:         可读字节数 = write_pos - read_pos
```

消息帧 `[MsgHeader 8B][payload]` 由 codec 层（第 7 节）在字节流之上定义。早期版本的"8 字节对齐帧 + `UINT32_MAX` 回绕哨兵"设计已经移除：回绕直接由"两段 `memcpy`"或"镜像映射单次 `memcpy`"处理。

### 4.5 回绕处理

写位置接近物理末尾、数据跨越环尾时：

```
普通映射 (TryWrite / TryRead / ReadExact / Peek):
  phys_w = write_pos & mask;  tail = Capacity - phys_w
  len <= tail:  1 次 memcpy
  len >  tail:  memcpy(base+phys_w, src, tail) + memcpy(base, src+tail, len-tail)
  Peek 回绕时返回两段 (seg1, seg2)

镜像映射 (TryWriteContig / TryReadContig / ReadExactContig / PeekContig):
  memcpy(base + phys_w, src, len)   ← 恒为 1 次，越过环尾的部分落在第二圈 VA
  PeekContig 永远返回单段
```

`RingChannel` 根据 `MmapRegion::Mirrored()` 自动选择对应路径，调用方无需关心。

### 4.6 内存序分析

这是整个系统正确性的关键。SPSC 模型下只有两个线程（进程），内存序要求最小化：

```
写方 (Producer)                    读方 (Consumer)
─────────────                     ─────────────
read  write_pos  [relaxed]  ①
read  read_pos   [acquire]  ②    ── 与 ⑥ 同步
                                  read  read_pos   [relaxed]  ④
写入 payload (memcpy)       ③    read  write_pos  [acquire]  ⑤ ── 与下一行同步
store write_pos  [release]  ③
                                  读取 payload (memcpy)       ⑤
                                  store read_pos   [release]  ⑥
```

| 操作                 | 内存序      | 原因                                                   |
| -------------------- | ----------- | ------------------------------------------------------ |
| 写方读 `write_pos` | `relaxed` | 仅写方修改此变量，无竞争                               |
| 写方读 `read_pos`  | `acquire` | 必须看到读方最新的 release store，确保已释放的空间可用 |
| 写方写 `write_pos` | `release` | payload 的 memcpy 必须在 write_pos 更新前对读方可见    |
| 读方读 `read_pos`  | `relaxed` | 仅读方修改此变量，无竞争                               |
| 读方读 `write_pos` | `acquire` | 必须看到写方 release store 之前的所有写入（payload）   |
| 读方写 `read_pos`  | `release` | 通知写方空间已释放                                     |

按需通知（4.8）额外引入一对 `seq_cst` 栅栏，用于在 `write_pos` 与 `reader_wait` 之间建立全序，见 4.8。

**在 x86-64 上的实际开销**：x86 的 TSO（Total Store Order）模型下，`acquire` 和 `release` 不需要额外的内存屏障指令，编译器只需防止指令重排即可。因此在 x86 上，这些原子操作的性能接近普通内存访问。

### 4.7 BatchWriter — 批量写入与 Reserve 直写

单次 `TryWrite` 需要 3 次原子操作（load `write_pos`、load `read_pos`、store `write_pos`）。`BatchWriter` 在构造时快照一次 `read_pos`，写入期间只在本地追踪 `write_pos`，`Flush` 时一次 release store，把 N 次写入的原子操作降到 2：

```cpp
RingChannel::ChannelBatchWriter batch;
if (ch.StartBatch(&batch) == ErrorCode::kOk)
{
    batch.TryWrite(hdr, 8);
    batch.TryWrite(payload, n);
    batch.Flush();            // store write_pos [release] + NotifyPeer()
}
```

要点：

- **同一环上同一时刻只允许一个活跃的 BatchWriter**：每个实例都从共享内存快照 `write_pos`，两个并存的实例会写到同一物理位置并互相覆盖
- `read_pos` 快照是保守的：读方只会消费更多、不会更少，因此不会误放行，只可能在读方已前进时误判空间不足
- `Reserve(len)` / `CommitReserve(len)`：预留连续区域并返回直写指针（镜像映射下跨环尾也成功），调用方原地构造数据后提交，省掉一次中间拷贝；release 构建下也校验"已 Reserve 且长度一致"
- `Cancel()` / `RewindTo(pending, count)`：回滚未发布的写入（对端不可见），供 codec 层做异常安全
- moved-from 对象是惰性的：`TryWrite`/`Reserve` 直接返回失败，不会误写共享环

### 4.8 reader_wait — 按需通知

读方处理过程中本来就在持续消费，写方每条消息都 `write(eventfd)` 是浪费。`reader_wait` 让写方只在读方**即将睡眠**时才发一次通知：

| 取值                     | 含义                                                   |
| ------------------------ | ------------------------------------------------------ |
| `kReaderAwake = 0`       | 读方在处理中，写方无需通知                             |
| `kReaderWaiting = 1`     | 读方即将/已经睡眠，写方需通知一次（通知时清零）        |
| `kReaderNotifyAlways = 2` | 初始值：读方未启用按需通知，写方每次都通知（兼容旧读方） |

```
读方 (ArmReaderWait)                 写方 (ShouldNotify)
────────────────────                 ────────────────────
                                     store write_pos [release]   ← 发布数据
store reader_wait = Waiting          fence(seq_cst)
fence(seq_cst)                       load  reader_wait
load  write_pos  → 与基线比较         Waiting → exchange(Awake)，抢到者通知
                                     Awake   → 不通知
  变了 → 撤销标志，继续读，不睡
  没变 → 可以睡眠 (poll)
```

两端各自"先写、栅栏、再读对方变量"，构成 Dekker 式配对：两边至少有一方能看到对方的写入，因此**不会丢唤醒**。

两条必须遵守的规则：

1. 读方的基线 `seen_write` 必须在**读环之前**取（`RingChannel::WritePos()`）。如果在读完之后才取，期间到达的数据会被算进基线，复查时看不出变化，读方带着未处理的数据睡去
2. `PrepareWait` 返回 `false` 表示有新数据，不得睡眠，应继续读取

从未调用过 `PrepareWait` 的读方（如 `src/server.cpp`、`src/client.cpp`、`test_codec`），标志保持初值 `kReaderNotifyAlways`，写方每次都通知，行为与引入 `reader_wait` 之前一致。

### 4.9 Peek / CommitRead — 零拷贝读取

```cpp
// Peek：返回环内数据的原始指针，不推进 read_pos
//   普通映射：数据回绕时返回两段 (seg1, seg2)
//   镜像映射：PeekContig 永远单段
static int Peek(const void *shm, const void **seg1, uint32_t *seg1_len,
                const void **seg2, uint32_t *seg2_len);

// CommitRead：确认消费完毕，推进 read_pos
static void CommitRead(void *shm, uint32_t len);
```

```
TryRead（拷贝读）:  ring → memcpy → user_buf → 处理
Peek（零拷贝）:     ring → 直接处理（指针有效直到 CommitRead）
```

零拷贝路径减少一次 `memcpy`。`CommitRead` 的 `len` 不得超过可读字节数：debug 构建 `assert`，release 构建钳制到实际可读字节数，避免破坏 `read_pos <= write_pos` 的不变式。

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

`RingChannel` 可移动、不可复制，默认构造后处于"未初始化"状态：读写接口返回 `-1` / `0`，`StartBatch` 返回 `kNotInitialized`。

### 5.2 四步握手协议

握手由统一入口 `RingChannel::CreateRingChannel(socket_fd, role, &out, &sys_errno)` 完成：客户端传 `ChannelRole::kClient`，服务端传 `ChannelRole::kServer`，两端互为镜像，角色只决定收发顺序。通过 Unix domain socket 交换 4 个文件描述符：

```
      Client                         Server
        │                              │
        │  1. SendFd(memfd_c2s)       │
        │ ─────────────────────────→  │  RecvFdExpect(kMemfd)
        │                              │  fstat 校验尺寸 → mmap → read_region_
        │  2. SendFd(efd_client)      │
        │ ─────────────────────────→  │  RecvFdExpect(kEventfd)
        │                              │  → notify_write_efd_
        │                              │
        │                              │  创建 memfd_s2c, mmap
        │                              │  创建 efd_server
        │                              │
        │  3. RecvFdExpect(kMemfd)    │
        │ ←─────────────────────────  │  SendFd(memfd_s2c)
        │  fstat 校验 → mmap          │
        │                              │
        │  4. RecvFdExpect(kEventfd)  │
        │ ←─────────────────────────  │  SendFd(efd_server)
        │  → notify_write_efd_        │
        │                              │
     握手完成，双向通道建立              握手完成
```

握手期间的几处防护：

- **超时**：`HandshakeGuard` 在握手期间给 socket 设 5 秒收发超时，防止半连接挂死；返回时（含出错路径）恢复阻塞模式
- **尺寸校验**：收到对端 memfd 后**先 `fstat`，比对 `Ring::shm_size` 再 `mmap`**。两端 `Capacity` 不一致，或对端发送 0 字节/被截断的 fd，直接返回 `kSizeMismatch`，而不是之后在环尾访问时 `SIGBUS`/`SIGSEGV`
- **先初始化后发送**：`Ring::Init` 必须在发送 memfd 之前完成——`SCM_RIGHTS` 的 `sendmsg`/`recvmsg` 提供 happens-before，使 `Init` 的清零对端可见
- **原子替换**：握手在局部对象上完成，成功才移动给出参 `out`，失败时 `out` 保持原状

### 5.3 eventfd 通知机制

数据路径（环形缓冲区读写）不经过 socket，但需要一种机制让对端知道"有新数据到达"。这通过 `eventfd` 实现：

```cpp
// 写方：数据已发布（write_pos 已 store）之后调用
void NotifyPeer() const
{
    if (!notify_write_efd_) return;                       // 未握手 / moved-from
    if (write_region_ && !Ring::ShouldNotify(write_region_.Get()))
        return;                                           // 读方在处理中，省掉一次 syscall
    uint64_t v = 1;
    do { r = ::write(notify_write_efd_.Get(), &v, sizeof(v)); }
    while (r < 0 && errno == EINTR);
}

// 读方：通过 poll 监听 eventfd
int NotifyReadFd() const noexcept { return notify_read_efd_.Get(); }

// 读方回调中排空 eventfd
static uint64_t DrainNotify(int efd);

// 读方：睡眠前声明（4.8）。返回 false 表示有新数据，不得睡眠
bool PrepareWait(uint64_t seen_write);
uint64_t WritePos() const;    // 取基线：必须在读环之前调用
uint64_t ReadPos() const;
```

读方的标准循环（`ShmConnection::DrainRing` 即是如此）：

```cpp
do {
    uint64_t seen = ch.WritePos();     // 基线：先于读环
    DrainOnce();                       // 读空当前所有数据
} while (!ch.PrepareWait(seen));       // 期间有新数据 → 继续读
// 回到 poll，等待 eventfd
```

eventfd 的特性使其非常适合此场景：

- **轻量**：仅维护一个 64 位计数器，`write` 累加，`read` 归零并返回
- **可 poll/epoll/select**：可集成到事件循环（EventLoop 基于 poll）
- **`EFD_NONBLOCK`**：非阻塞模式，无数据时 `read` 返回 `EAGAIN`
- **`EFD_CLOEXEC`**：exec 时自动关闭，防止 fd 泄露

### 5.4 为何 eventfd 优于其他通知方案

| 方案          | 延迟    | fd 数量       | 实现复杂度        |
| ------------- | ------- | ------------- | ----------------- |
| eventfd       | ~1μs   | 1             | 低                |
| pipe          | ~1μs   | 2 (读端+写端) | 中                |
| Unix socket   | ~2μs   | 1             | 高                |
| 信号 (SIGUSR) | 不确定  | 0             | 高，不可靠        |
| futex         | ~0.5μs | 0             | 高，需共享内存    |
| 忙轮询        | ~0      | 0             | 最低，但 CPU 100% |

项目选择 eventfd：单 fd、可 poll、低延迟、实现简单；再配合 `reader_wait`，写方只在读方睡眠时才付出这次 syscall。

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
// common.hpp
[[nodiscard]]
inline ErrorCode SendFd(int socket, int fd, FdTag tag, int *sys_errno = nullptr) noexcept
{
    // iovec 载荷：256 字节，前 4 字节为 FdTag
    std::array<char, 256> dummy{};
    uint32_t tag_val = static_cast<uint32_t>(tag);
    std::memcpy(dummy.data(), &tag_val, sizeof(tag_val));

    // SOCK_STREAM 允许短写：SCM_RIGHTS 只随第一条 sendmsg 携带，
    // 剩余载荷用不带控制消息的 sendmsg 循环补齐，保证对端流对齐
    std::size_t sent = 0;
    while (sent < dummy.size())
    {
        iovec io{dummy.data() + sent, dummy.size() - sent};
        msghdr msg{};  msg.msg_iov = &io;  msg.msg_iovlen = 1;
        if (sent == 0)  // 仅首条消息携带 fd
        {
            /* 填充 cmsg: SOL_SOCKET / SCM_RIGHTS / CMSG_LEN(sizeof(int)) */
        }
        ssize_t n;
        do { n = ::sendmsg(socket, &msg, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
        if (n < 0)  return ErrnoToErrorCode(errno);
        if (n == 0) return ErrorCode::kPeerClosed;
        sent += n;
    }
    return ErrorCode::kOk;
}
```

`MSG_NOSIGNAL` 使对端已关闭时返回 `EPIPE` 而不是向本进程投递 `SIGPIPE`。

### 6.3 RecvFd：短读、截断与多 fd

`RecvFd` 需要防御几类协议问题，失败时已接收的 fd 由 `UniqueFd` 自动关闭，不会泄漏：

| 情形                                   | 处理                                            |
| -------------------------------------- | ----------------------------------------------- |
| `SOCK_STREAM` 短读                     | 循环读满 256 字节载荷；`SCM_RIGHTS` 随首字节到达，只解析一次 |
| 控制消息被截断（`MSG_CTRUNC`）         | `kProtocol`                                     |
| 缺少 `SCM_RIGHTS` / 控制消息过短       | `kProtocol`                                     |
| 一条消息携带多个 fd                    | 关闭全部 fd，返回 `kMultipleFds`                |
| 对端关闭                               | `kPeerClosed`                                   |

不做短读处理的话，残留的 dummy 字节会被下一次 `RecvFd` 当作数据读走，后续消息的 `SCM_RIGHTS` 丢失或标签错位。

### 6.4 FdTag 类型安全校验

每次 fd 传递都附带一个 `FdTag` 标签，接收方通过 `RecvFdExpect` 校验：

```cpp
enum class FdTag : uint32_t
{
    kMemfd   = 0x4D454D46,  // "MEMF" 的 ASCII
    kEventfd = 0x45564644,  // "EVFD" 的 ASCII
};
```

这防止握手时序错乱——如果客户端和服务端的发送/接收顺序不一致，`RecvFdExpect` 会立即返回 `kProtocol`（并关闭收到的 fd），而不是静默地把 eventfd 当作 memfd 使用（这会导致难以排查的内存损坏）。

---

## 7. Codec — 帧编解码

### 7.1 帧格式

`codec.hpp` 是纯字节流 codec，只负责 `[MsgHeader 8B][payload]` 帧；类型标识（POD tag、protobuf type_name 等）由上层 codec 写入 payload 内部：

```
┌──────────────────┬──────────────────────────┐
│ MsgHeader 8B     │ payload (N bytes)        │
│ len u32 | seq u32│                          │
└──────────────────┴──────────────────────────┘
len = payload 字节数（不含 MsgHeader）
```

没有对齐填充：帧在环里首尾相接，前一帧的最后一个字节后面紧跟下一帧的 `MsgHeader`。

### 7.2 上层类型编码

| codec                  | payload 布局                                             |
| ---------------------- | -------------------------------------------------------- |
| `PodCodec<T>`          | `[tag u32][T bytes]`，接收时校验 tag                    |
| `SimplePodCodec`       | `[tag u32][T bytes]`，`std::any` 模型的通用 POD codec    |
| `ProtoCodec<T>`        | `[type_name_len u16][type_name][pb bytes]`              |

POD 类型通过 `SHM_IPC_REGISTER_POD` 宏与 4 字节标签绑定：

```cpp
struct Heartbeat { int32_t client_id; int32_t seq; int64_t timestamp; };
SHM_IPC_REGISTER_POD(Heartbeat, 0x48425454)  // "HBTT"
```

宏展开后特化 `TypeTag<Heartbeat>`，同时 `static_assert` 强制要求类型满足 `std::is_trivially_copyable_v`。未注册的类型在编译期即报错。

### 7.3 发送接口

```cpp
// 帧头 + 回调写 payload，自动 Flush + NotifyPeer
template <std::size_t Cap, typename WriteFn>
int SendFrame(RingChannel<Cap> &ch, uint32_t payload_len, uint32_t seq, WriteFn &&write_payload);

// 同上，但写入已有的 ChannelBatchWriter，不 Flush，由调用方统一发布
template <std::size_t Cap, typename WriteFn>
int SendFrameBatch(typename RingChannel<Cap>::ChannelBatchWriter &batch,
                   uint32_t payload_len, uint32_t seq, WriteFn &&write_payload);

// 便捷接口：单块 payload（≤ max_write_size）
template <std::size_t Cap> int Send(RingChannel<Cap> &ch, const void *payload, uint32_t len, uint32_t seq);
template <std::size_t Cap> int Send(typename RingChannel<Cap>::ChannelBatchWriter &batch, ...);
```

返回 `0` 成功，`-1` 缓冲区满、消息过大或回调写入字节数与 `payload_len` 不符。

**安全性**：

- 帧长用 64 位计算，避免 `kMsgHeaderSize + payload_len` 在 `uint32_t` 溢出后绕过空间检查
- **异常安全**：`write_payload` 回调抛异常时，`SendFrame` 用 `Cancel()`、`SendFrameBatch` 用 `RewindTo()` 回滚未发布的写入后重新抛出。放任析构自动 `Flush` 会把 `[头][半截 payload]` 发布给对端，对端按伪造的 `len` 消费后续帧的字节，造成永久的流错位
- **长度校验**：回调实际写入的字节数必须恰好等于 `payload_len`，否则回滚并返回 `-1`，不发布残缺帧

### 7.4 FrameReader — 零拷贝流式读取器

`FrameReader<BufSize, MaxBufSize>` 用 `Peek` + `CommitRead` 从字节流中提取完整帧：

```cpp
template <std::size_t Cap>
int TryRecv(RingChannel<Cap> &ch, const void **payload, uint32_t *payload_len);
```

**快路径**：帧完整位于 `Peek` 返回的连续段内（镜像映射下恒成立）→ `payload` 直接指向 ring 内部，零拷贝。指针在**下一次 `TryRecv`** 之前有效（下次调用才会推进 `read_pos`，释放上一帧占用的空间）。

**慢路径**：非镜像映射下帧跨越环尾 → 拷贝到内部回退缓冲区。`BufSize`（默认 64KB）只是初始 `reserve`，缓冲区按需增长：发送端允许的大帧（可到 `Capacity/2`）不会因固定缓冲区被误判为毒帧。

返回值：

| 返回值 | 含义                                                                                 |
| ------ | ------------------------------------------------------------------------------------ |
| `0`    | 成功取得一帧                                                                         |
| `-1`   | 数据不足（帧头或 payload 尚未完整到达）                                              |
| `-2`   | 帧跨环尾且超过显式配置的 `MaxBufSize`（默认不设上限，因此默认不会出现）；该帧被标记为待丢弃，下次自动跳过 |
| `-3`   | 帧头 `len` 非法（超过 `Capacity - 8`，永远不可能完整到达）；协议错误，`read_pos` 不动，调用方应断开连接 |

---

## 8. 完整数据流示例

以客户端发送一条消息到服务端为例，展示从发送到接收的完整路径（原始通道层 API）：

```
Client 进程                                        Server 进程
─────────────                                     ─────────────

1. Send(channel, payload, len, seq)
   │
   └→ SendFrame
        │
        ├→ StartBatch：快照 write_pos [relaxed]、read_pos [acquire]
        │                  ←── 与 Server 的 release 同步
        ├→ batch.TryWrite(MsgHeader, 8)        本地推进，不触发原子 store
        ├→ batch.TryWrite(payload, len)        镜像映射下跨环尾也只 1 次 memcpy
        │
        └→ batch.Flush()
             ├ store write_pos [release] ──→ 发布新数据
             └ NotifyPeer()
                  ├ ShouldNotify: fence(seq_cst); 读 reader_wait
                  │    ├ Awake  → 不通知（读方在处理中，随后会看到数据）
                  │    └ Waiting→ exchange(Awake)，抢到者继续
                  └ write(notify_write_efd_, 1)
                       │
                       │  ────── eventfd 内核通知 ──────
                       │                                    │
                       │                              2. poll() 触发 POLLIN
                       │                                 │
                       │                              3. DrainNotify(efd)
                       │                                 read(efd) → 清除计数
                       │                                 │
                       │                              4. 读方循环：
                       │                                 do {
                       │                                   seen = WritePos()   ← 先取基线
                       │                                   FrameReader::TryRecv(channel, &p, &n)
                       │                                     ├→ Peek(read_region_)
                       │                                     │    ├ 读 read_pos  [relaxed]
                       │                                     │    ├ 读 write_pos [acquire] ←── 看到 Client 的 release
                       │                                     │    └ 返回 ring 内部指针（单段）
                       │                                     └→ 解析 MsgHeader，payload 零拷贝
                       │                                   处理 payload
                       │                                   (下次 TryRecv 内部 CommitRead：
                       │                                    store read_pos [release] ──→ 释放空间)
                       │                                 } while (!PrepareWait(seen))
                       │                                      ├ reader_wait = Waiting; fence(seq_cst)
                       │                                      ├ write_pos 变了 → 撤销，继续读
                       │                                      └ 没变 → 回到 poll 睡眠
```

---

## 9. 性能特征

### 9.1 延迟分析

| 阶段                 | 操作                              | 预期延迟                           |
| -------------------- | --------------------------------- | ---------------------------------- |
| 编码                 | 帧头 + payload 各 1 次 memcpy     | < 100ns (L1 命中)                  |
| 写入                 | atomic store + memcpy             | < 200ns                            |
| 通知                 | eventfd write（仅读方睡眠时发生） | ~1μs (系统调用)                   |
| 唤醒                 | poll 返回                         | ~1-5μs                            |
| 读取                 | atomic load + 零拷贝 Peek         | < 200ns                            |
| **端到端 RTT** |                                   | **~2-10μs（读方睡眠，需 eventfd 唤醒）** |
| **忙轮询 RTT** |                                   | **~200-500ns（无 eventfd）** |

实测数据见 `bench/results/`：`2026-11-10_shm_vs_socket` 中 64B ping-pong RTT 约 450ns，Unix socket 约 10µs。

### 9.2 吞吐量

端到端吞吐（`bench/results/2026-11-10_shm_vs_socket`，-O2，中位数）：64B 约 3000 万 msg/s（约 1.9 GB/s），16KB 约 24.8 GB/s，大消息（≥1MB）受内存带宽限制。相对 Unix socket，小包领先约 45–49 倍，1–2MB 约 1.6–1.7 倍。

读方持续忙碌（有积压）时，`reader_wait` 使写方只在读方睡眠时才写 eventfd，忙碌期间省掉每条消息一次的 `write` syscall；batch=1（ping-pong）读方每次都要睡眠，无变化。具体对比见 `bench/results/` 中 `bench_app` 的 batch 扫描日志。

### 9.3 相比传统 IPC 的优势

```
┌──────────────────────────────────────────────────────────────────┐
│ Unix socket:  用户态 → 系统调用 → 内核缓冲区 → 系统调用 → 用户态   │
│               send()     copy      copy       recv()             │
│               至少 2 次内核拷贝 + 2 次系统调用                      │
├──────────────────────────────────────────────────────────────────┤
│ RingChannel: 用户态 → mmap 共享页 → 用户态                        │
│              memcpy (1次)    atomic load                          │
│              0 次内核拷贝, 数据路径无系统调用（唤醒除外）            │
└──────────────────────────────────────────────────────────────────┘
```

---

## 10. 为什么 write_pos / read_pos 必须是原子变量

`RingHeader` 中的 `write_pos` 和 `read_pos` 声明为 `std::atomic<uint64_t>` 而非普通 `uint64_t`，解决两个具体问题。

### 10.1 防止撕裂读写（Tearing）

`write_pos` 和 `read_pos` 是 8 字节的 `uint64_t`。在 32 位系统上，一次普通的 64 位写入会被拆成两条 32 位指令：

```
普通 uint64_t 写入（32 位系统）:
  store [addr],   low_32    ← 先写低 4 字节
  store [addr+4], high_32   ← 再写高 4 字节

如果读方在两条指令之间读取：
  读到：高位是旧值，低位是新值 → 得到一个从未存在过的中间值
```

`std::atomic<uint64_t>` 保证 load/store 是不可分割的，不会读到半新半旧的值。

在 64 位 x86 上，对齐的 64 位读写本身就是原子的，`atomic` 的 tearing 防护开销为零。但代码不应依赖平台假设。库里用 `static_assert(std::atomic<uint64_t>::is_always_lock_free)` 强制要求 lock-free：共享内存上的原子变量若退化为内部互斥锁，跨进程使用会直接死锁。

### 10.2 控制内存可见性顺序

这是使用原子变量的核心原因。两个进程各自有独立的 CPU 缓存和 store buffer，**没有原子操作的内存序约束，一个进程的写入对另一个进程不保证可见，也不保证顺序**。

```
写方 CPU                              读方 CPU
─────────                             ─────────
memcpy(payload)      ①               
write_pos = new_val  ②                 load write_pos  → 看到 new_val
                                       memcpy(payload)  → 可能看到旧数据！
```

问题：CPU 和编译器都可能重排指令。如果 ② 被提前到 ① 之前执行（或 ① 的结果还在 store buffer 中未刷出），读方看到了新的 `write_pos` 但 payload 数据还没就绪——读到脏数据。

`atomic` + `release/acquire` 建立了先行发生（happens-before）关系：

```
写方                                  读方
─────                                ─────
memcpy(payload)                    
write_pos.store(release)  ③          write_pos.load(acquire)  ⑤
       │                                    │
       │  release 保证：                     │  acquire 保证：
       │  ③ 之前的所有写入                   │  ⑤ 之后的所有读取
       │  不会被重排到 ③ 之后                │  不会被重排到 ⑤ 之前
       │                                    │
       └──── 当 ⑤ 看到 ③ 的值时 ────────────┘
              payload 一定已经对读方可见
```

### 10.3 如果不用 atomic 会怎样

假设改成普通 `uint64_t`：

```cpp
uint64_t write_pos;   // 非 atomic
uint64_t read_pos;    // 非 atomic
```

两个后果：

1. **编译器重排**：编译器认为 `write_pos = ...` 和前面的 `memcpy` 没有数据依赖，可能将赋值提前到 memcpy 之前执行——读方看到新 write_pos 时 payload 还未写入。
2. **CPU 重排 + 缓存不可见**：即使编译器没重排，CPU 的 store buffer 和缓存一致性协议也不保证写入顺序对其他核心可见。写方的 `memcpy` 结果可能还躺在 store buffer 里，读方就已经看到了更新的 `write_pos`。

`atomic` 同时解决这两层问题——既阻止编译器重排，又插入必要的硬件屏障（或利用硬件的内存模型保证）。

### 10.4 在 x86 上的实际开销

x86 是 TSO（Total Store Order）架构，硬件保证：

- 所有 store 按程序顺序对其他核心可见
- 所有 load 按程序顺序执行

因此 `acquire` load 和 `release` store **不需要额外的硬件屏障指令**，编译器只需禁止自身的指令重排即可。在 x86 上，`atomic` + `acquire/release` 的性能与普通变量访问几乎相同——这是零成本的正确性保障。

唯一的例外是 `reader_wait` 握手用到的 `seq_cst` 栅栏（4.8）：它要求"先 store、后 load 另一个变量"也保持顺序，而 TSO 允许 store→load 重排，所以 x86 上会生成真正的栅栏指令（`mfence` 或带 `lock` 前缀的指令）。这一点开销出现在每次发布数据后（`ShouldNotify`）和读方每次睡眠前（`ArmReaderWait`），不在每次读写的热路径上逐字节累加。

在 ARM/RISC-V 等弱内存序架构上，`acquire` 和 `release` 会生成对应的屏障指令（如 ARM 的 `dmb` / `ldar` / `stlr`），存在少量性能开销，但这是弱内存序架构上正确性的必要代价。

---

## 11. 源文件索引

| 文件                                      | 职责                                                                                                |
| ----------------------------------------- | --------------------------------------------------------------------------------------------------- |
| `include/shm_ipc/error.hpp`             | `ErrorCode` 统一错误码、`ErrnoToErrorCode` errno 归类                                           |
| `include/shm_ipc/common.hpp`            | `UniqueFd`, `MmapRegion`（含 `MapMirrored`）, `CreateMemfd`, `SendFd`/`RecvFd`, `CreateEventfd`, Unix socket 辅助 |
| `include/shm_ipc/ringbuf.hpp`           | `RingBuf<Cap>` — 无锁 SPSC 字节环、`BatchWriter`、`reader_wait` 按需通知                          |
| `include/shm_ipc/ring_channel.hpp`      | `RingChannel<Cap>` — 双向通道封装，fd 握手协议，`ChannelBatchWriter`，`PrepareWait`               |
| `include/shm_ipc/codec.hpp`             | `MsgHeader`, `Encode`/`Decode`, `Send`/`SendFrame`/`SendFrameBatch`, `SHM_IPC_REGISTER_POD`      |
| `include/shm_ipc/frame_reader.hpp`      | `FrameReader` — 零拷贝流式帧提取（Peek + CommitRead）                                            |
| `include/shm_ipc/pod_codec.hpp`         | `PodCodec<T>` — POD 消息 codec，payload = `[tag u32][T bytes]`                                   |
| `include/shm_ipc/simple_pod_codec.hpp`  | `codec::SimplePodCodec` — `std::any` 模型的通用 POD codec                                         |
| `include/shm_ipc/proto_codec.hpp`       | `ProtoCodec<T>` — protobuf 消息 codec（需要 protobuf）                                            |
| `include/shm_ipc/messages.hpp`          | `Heartbeat`, `ClientMsg` 应用层 POD 消息定义                                                      |
| `include/shm_ipc/event_loop.hpp`        | `EventLoop` — poll + timerfd 事件循环                                                              |
| `include/shm_ipc/client_state.hpp`      | `ClientState` — 连接状态聚合                                                                       |
| `include/shm_ipc/shm_connection.hpp` 等 | 上层接口：`ShmConnection` / `ShmServer` / `ShmAsyncClient` / `ShmClient`（实现 `include/comm/` 接口）|
| `src/server.cpp`                        | 多客户端服务端，EventLoop 驱动                                                                      |
| `src/client.cpp`                        | 客户端，定时发送 + eventfd 接收                                                                     |
| `bench/bench_shm.cpp`                   | fork + socketpair 回声延迟基准测试                                                                  |
| `bench/bench_app.cpp`                   | 上层接口（ShmServer + ShmAsyncClient）按批吞吐基准                                                  |
