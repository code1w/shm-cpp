# RingBuf 字节流环形缓冲区实现详解

本文档面向初次接触本项目的开发者，详细讲解 `RingBuf` 纯字节流 SPSC 环形缓冲区的实现原理，涵盖共享内存布局、位置管理、写入与读取流程、回绕（wrap-around）机制、镜像映射、内存序语义、批量写入优化以及 `reader_wait` 按需通知。

---

## 1. 共享内存布局

```
共享内存 (kShmSize = kRingDataOffset + Capacity = 4096 + Capacity 字节)
┌──────────────────────────────────────────────────────────────┐
│               RingHeader (192 bytes, 3 个缓存行)              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ write_pos    atomic<u64>  + 56B padding   (偏移 0)     │  │
│  ├────────────────────────────────────────────────────────┤  │
│  │ read_pos     atomic<u64>  + 56B padding   (偏移 64)    │  │
│  ├────────────────────────────────────────────────────────┤  │
│  │ reader_wait  atomic<u32>  + 60B padding   (偏移 128)   │  │
│  └────────────────────────────────────────────────────────┘  │
│            填充至 4096 字节（kRingDataOffset，页对齐）         │
├──────────────────────────────────────────────────────────────┤
│                Data Region (Capacity bytes)                   │
│  偏移 4096                                    偏移 4096+Cap-1 │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │ b b b b b b b b b b b b b b b ··· b b b b b b b b b b b│ │
│  └──────────────────────────────────────────────────────────┘ │
│  ↑                                                 ↑         │
│  phys_r (读位置)                            phys_w (写位置)   │
└──────────────────────────────────────────────────────────────┘
```

数据区起点页对齐（4096）是镜像映射的前提，见第 11 节。

### 两个核心字段

| 字段            | 所在缓存行             | 写方                             | 读方                        |
| --------------- | ---------------------- | -------------------------------- | --------------------------- |
| `write_pos`   | 第 1 缓存行 (偏移 0)   | 写入数据后 **store**       | 检测新数据时 **load** |
| `read_pos`    | 第 2 缓存行 (偏移 64)  | 检测空间时 **load**        | 消费数据后 **store**  |
| `reader_wait` | 第 3 缓存行 (偏移 128) | 发布后 **load / exchange** | 睡眠前 **store**      |

`reader_wait` 不参与数据同步，只用于按需通知，见第 10 节。三个字段各占独立缓存行（`alignas(64)` + padding），避免 **false sharing**——写方修改 `write_pos` 不会使读方的 `read_pos` 所在缓存行失效，反之亦然；读方睡眠标志的翻转也不会干扰这两个热点变量。

---

## 2. 位置管理：逻辑偏移与物理偏移

### 单调递增的逻辑偏移

`write_pos` 和 `read_pos` 都是 **单调递增的 64 位字节偏移**，永远不回绕归零。例如一个 Capacity=4096 的环，写入 5000 字节后 `write_pos=5000`，不是 `5000 % 4096 = 904`。

这样设计的好处：

- **已用空间** = `write_pos - read_pos`，一次减法即可，无需处理回绕
- **空闲空间** = `Capacity - (write_pos - read_pos)`
- 64 位永不溢出（以 10GB/s 持续写入也要 58 年才溢出）

### RingPhys：逻辑偏移 → 物理偏移

```cpp
static constexpr std::size_t RingPhys(uint64_t pos)
{
    return static_cast<std::size_t>(pos & (Capacity - 1));
}
```

因为 Capacity 必须为 2 的幂，`pos & (Capacity - 1)` 等价于 `pos % Capacity`，但只需一条 AND 指令。

示例（Capacity = 4096 = 0x1000）：

```
pos = 5000  →  RingPhys(5000) = 5000 & 0xFFF = 904   ← 物理偏移
pos = 4096  →  RingPhys(4096) = 4096 & 0xFFF = 0      ← 刚好回到起点
pos = 8192  →  RingPhys(8192) = 8192 & 0xFFF = 0      ← 第二圈起点
```

### 可用空间检查

```
已用 = w - r               （单调递增，直接相减）
可用容量 = Capacity - (w - r)
写入条件: (w + len) - r <= Capacity   即 已用 + 新增 <= 总容量
```

`kMaxWriteSize = Capacity / 2` 限制单次最大写入为容量的一半。这简化了空间管理——无论读写位置如何，一次 `kMaxWriteSize` 的写入只要空闲超过一半就一定能成功。编译期还要求 Capacity 为 2 的幂、不小于 1024、不超过 `UINT32_MAX`。

---

## 3. TryWrite 写入流程

```cpp
static int TryWrite(void *shm, const void *data, uint32_t len)
```

### 完整流程图

```
        TryWrite(shm, data, len)
                  │
                  ▼
    ┌─ len == 0 || len > kMaxWriteSize? ──── 是 ──→ return -1
    │             否
    │             ▼
    │   w = write_pos.load(relaxed)        ← 读自己的位置，relaxed 够用
    │   r = read_pos.load(acquire)         ← 读对方的位置，acquire 同步
    │             │
    │   (w + len) - r > Capacity? ──────── 是 ──→ return -1  (空间不足)
    │             否
    │             ▼
    │   phys_w = w & (Capacity - 1)        ← 逻辑 → 物理偏移
    │   tail   = Capacity - phys_w         ← 到环尾的连续字节数
    │             │
    │   len <= tail? ─────────── 是 ──→ 单段拷贝: memcpy(base+phys_w, src, len)
    │       否                                         │
    │       ▼                                          │
    │   两段拷贝:                                      │
    │     memcpy(base+phys_w, src, tail)               │
    │     memcpy(base, src+tail, len-tail)             │
    │       │                                          │
    │       ▼◀─────────────────────────────────────────┘
    │   write_pos.store(w + len, release)  ← 发布新数据
    │             │
    └─────────── return 0
```

### 逐步解析

**① 前置校验**

```cpp
if (len == 0 || len > kMaxWriteSize)
    return -1;
```

空写入无意义；过大写入可能套圈 read_pos。

**② 加载位置**

```cpp
uint64_t w = hdr->write_pos.load(std::memory_order_relaxed);
uint64_t r = hdr->read_pos.load(std::memory_order_acquire);
```

- `write_pos` 用 **relaxed**：只有本线程/进程写它，读自己的数据不需要同步。
- `read_pos` 用 **acquire**：需要看到读方最新消费进度，acquire 保证在此之前读方的 `release store` 可见。

**③ 空间检查**

```cpp
if ((w + len) - r > Capacity)
    return -1;
```

逻辑距离 `w - r` 是当前已用量，加上 `len` 如果超过 `Capacity` 则空间不足。

**④ 数据拷贝（核心）**

```cpp
std::size_t phys_w = RingPhys<Capacity>(w);
std::size_t tail   = Capacity - phys_w;
```

`tail` 是从物理写位置到环形缓冲区末尾的连续可写字节数。

**情况 A：不回绕（len ≤ tail）**

```
  物理偏移:  0                    phys_w          phys_w+len     Cap-1
             ├────── 已读区 ──────┤▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓├──── 空闲 ────┤
                                  ↑ 单次 memcpy ↑
```

一次 `memcpy` 即可。

**情况 B：回绕（len > tail）**

```
  物理偏移:  0       len-tail     phys_w                        Cap-1
             ├▓▓▓▓▓▓▓├── 空闲 ──┤▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓┤
              第二段 memcpy        第一段 memcpy
              (src+tail 开始)      (src 开始, 写 tail 字节)
```

数据跨越环尾，拆分为两次 `memcpy`：

1. `memcpy(base + phys_w, src, tail)` — 填满从写位置到环尾
2. `memcpy(base, src + tail, len - tail)` — 剩余写到环头部

**关键保证**：两次 memcpy 都在 `write_pos.store` **之前**完成。读方只有看到新的 `write_pos` 才会去读这些字节，此时数据已完整写入。

**⑤ 发布**

```cpp
hdr->write_pos.store(w + len, std::memory_order_release);
```

**release store** 保证：所有先前的 memcpy 写入在此 store 之前对其他线程/进程可见。读方通过 `acquire load write_pos` 看到新值时，数据一定已经就位。

---

## 4. ReadExact 精确读取流程

```cpp
static int ReadExact(void *shm, void *data, uint32_t len)
```

### 完整流程图

```
        ReadExact(shm, data, len)
                  │
                  ▼
    r = read_pos.load(relaxed)             ← 读自己的位置
    w = write_pos.load(acquire)            ← 读对方的位置，acquire 同步
    avail = w - r
                  │
    avail < len? ───── 是 ──→ return -1  (数据不足，不消费任何字节)
        否
        ▼
    CopyOut(base, RingPhys(r), data, len)  ← 处理回绕拷贝
        │
    read_pos.store(r + len, release)       ← 释放已消费空间
        │
    return 0
```

### CopyOut 内部

```cpp
static void CopyOut(const char *base, std::size_t phys_r,
                    void *dst, uint32_t len)
{
    std::size_t contig = Capacity - phys_r;  // 到环尾的连续可读字节数
    if (len <= contig)                       // 不回绕
        memcpy(dst, base + phys_r, len);
    else                                     // 回绕：两段拷贝
    {
        memcpy(dst, base + phys_r, contig);           // 环尾部分
        memcpy(dst + contig, base, len - contig);     // 环头部分
    }
}
```

**关键特性**：如果可用数据不足 `len` 字节，`ReadExact` 直接返回 -1，**不消费任何字节**。这对上层 codec 解析至关重要——可以安全地反复调用直到完整帧到达。

---

## 5. TryRead 尽力读取

```cpp
static uint32_t TryRead(void *shm, void *data, uint32_t max_len)
```

与 `ReadExact` 的区别：

|            | ReadExact                                 | TryRead                               |
| ---------- | ----------------------------------------- | ------------------------------------- |
| 语义       | 要么读到恰好 `len` 字节，要么什么都不读 | 读最多 `max_len` 字节，有多少读多少 |
| 返回值     | 0 成功 / -1 失败                          | 实际读取的字节数（0 表示空）          |
| 不足时行为 | 不消费任何字节                            | 读取全部可用字节（可能 < max_len）    |
| 适用场景   | codec 层读定长帧                          | 原始字节流消费                        |

```
to_read = min(avail, max_len)
CopyOut(base, RingPhys(r), dst, to_read)  ← 同样的回绕处理
read_pos.store(r + to_read, release)
return to_read
```

---

## 6. 回绕（wrap-around）机制详解

回绕是环形缓冲区的核心难点。本实现采用**拆分拷贝**（split-copy），无需哨兵标记；在镜像映射下（第 11 节）则连拆分都不需要，本节描述的是普通映射下的路径。

### 6.1 为什么会回绕

物理缓冲区是一段连续内存 `[0, Capacity)`。当写位置到达尾部，下一次写入必须绕回到头部。

```
逻辑视角（无限长带）:    ···[已读][可读数据][待写空间]···
                              ↑r         ↑w

物理视角（环形）:   ┌──────────────────────────────┐
                    │ 待写 │ 可读 │    可读    │待写│
                    └──────────────────────────────┘
                    0      ↑phys_r         ↑phys_w  Cap
```

### 6.2 写入回绕示例

假设 `Capacity = 16`, `phys_w = 12`, 写入 `len = 8` 字节 `[A B C D E F G H]`:

```
写入前: phys_w = 12, tail = 16 - 12 = 4
        len(8) > tail(4) → 需要回绕

物理内存:
偏移:   0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
       [·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·]
                                             ↑ phys_w

第一段 memcpy: 写 tail=4 字节到环尾
       [·  ·  ·  ·  ·  ·  ·  ·  ·  ·  ·  · |A  B  C  D]

第二段 memcpy: 写 len-tail=4 字节到环头
       [E  F  G  H  ·  ·  ·  ·  ·  ·  ·  · |A  B  C  D]

write_pos += 8   (逻辑偏移推进，不回绕)
```

### 6.3 读取回绕示例

接上例，假设 `phys_r = 12`，读取 8 字节：

```
CopyOut: phys_r = 12, contig = 16 - 12 = 4
         len(8) > contig(4) → 需要回绕

第一段 memcpy: 从环尾读 4 字节 → dst[0..3] = [A B C D]
第二段 memcpy: 从环头读 4 字节 → dst[4..7] = [E F G H]

dst = [A B C D E F G H]   ✓ 与写入顺序完全一致
```

### 6.4 回绕正确性保证

回绕的正确性依赖两个关键事实：

1. **写入顺序 = 读取顺序**。`TryWrite` 的两段 memcpy 顺序是 `[尾部, 头部]`，`CopyOut` 同样按 `[尾部, 头部]` 顺序拼接到线性 buffer，逻辑顺序完全一致。
2. **内存序保证可见性**。写方的两段 memcpy 都在 `write_pos.store(release)` 之前完成；读方只有在 `write_pos.load(acquire)` 看到新值后才会去读数据——此时两段数据均已可见。

```
   写方                              读方
    │                                 │
  memcpy(尾部)                        │
  memcpy(头部)                        │
    │                                 │
  write_pos.store(release) ──同步──→ write_pos.load(acquire)
    │                                 │
    │                               CopyOut(尾部→头部)
    │                               read_pos.store(release)
```

### 6.5 回绕写入不会覆盖未读数据的证明

写入前的空间检查 `(w + len) - r <= Capacity` 保证了**空闲空间 >= len**。下面分两种物理布局严格证明回绕写入的第二段 `memcpy(base, src + tail, len - tail)` 不会覆盖未读数据。

#### 情况 1：phys_w >= phys_r

```
0                                          Capacity
|------[rrrrr未读数据rrrrr]----空闲----|----空闲----|
       ^phys_r              ^phys_w
   
空闲区域 = [phys_w, Capacity) + [0, phys_r)
         = (Capacity - phys_w) + phys_r
         = Capacity - (phys_w - phys_r)
         = Capacity - (w - r)          ← 此情况下物理差 = 逻辑差
         = free
```

当 `len > tail`（`tail = Capacity - phys_w`）时触发回绕：

- 第一段写 `tail` 字节，填满 `[phys_w, Capacity)`
- 第二段写 `len - tail` 字节，写入范围为 `[0, len - tail)`

第二段不覆盖未读数据的条件是 `len - tail <= phys_r`。证明：

```
空间检查保证:  free >= len
即:           Capacity - (w - r) >= len
此情况下:      Capacity - (phys_w - phys_r) >= len
展开:          (Capacity - phys_w) + phys_r >= len
即:            tail + phys_r >= len
              phys_r >= len - tail        ✅ 证毕
```

#### 情况 2：phys_w < phys_r（写指针已经 wrap 过读指针）

```
0                                          Capacity
|--已写--|------空闲------|--未读数据--|------已写----|
         ^phys_w         ^phys_r
     
空闲区域 = [phys_w, phys_r) = phys_r - phys_w
```

此时 `tail = Capacity - phys_w`，而空闲空间 `free = phys_r - phys_w`，所以：

```
len <= free = phys_r - phys_w < Capacity - phys_w = tail
```

即 `len < tail`，**根本不会进入 wrap 分支**——只走单段 memcpy，不存在覆盖问题。

#### 总结

| 物理布局             | 是否会触发回绕 | 安全性原因                                                    |
| -------------------- | -------------- | ------------------------------------------------------------- |
| `phys_w >= phys_r` | 可能会         | 空间检查保证 `len - tail <= phys_r`，第二段写不到未读区域   |
| `phys_w < phys_r`  | 不会           | 空闲空间在中间一整块，`len <= tail` 恒成立，只走单段 memcpy |

因此，只要空间检查 `(w + len) - r <= Capacity` 通过，回绕写入就绝不会覆盖未读数据。

### 6.6 边界条件

| 条件                            | 行为                                               |
| ------------------------------- | -------------------------------------------------- |
| `len == tail`                 | 刚好填满尾部，不回绕（走 `len <= tail` 分支）    |
| `len < tail`                  | 完全在尾部空间内，不回绕                           |
| `tail == 0` (`phys_w == 0`) | 整个写入在环头部，`len <= Capacity` 走不回绕分支 |
| `len == Capacity/2`           | 单次最大写入，最多占半个环                         |

---

## 7. Peek + CommitRead 零拷贝读取

当可读数据跨越环尾时，无法用单个指针表示。普通映射下 `Peek` 返回两段：

```cpp
static int Peek(const void *shm,
                const void **seg1, uint32_t *seg1_len,
                const void **seg2, uint32_t *seg2_len)
```

```
情况 A: 不回绕 (avail <= contig)

  偏移 0           phys_r    phys_r+avail            Cap-1
  ├─────── ··· ────┤▓▓▓▓▓▓▓▓▓├─────── ··· ──────────┤
                    seg1 ──────▶
                    seg2 = nullptr, seg2_len = 0


情况 B: 回绕 (avail > contig)

  偏移 0      avail-contig  phys_r                   Cap-1
  ├▓▓▓▓▓▓▓▓▓▓├──── ··· ────┤▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓┤
   seg2 ──────▶              seg1 ───────────────────▶
```

调用者拿到两段指针后自行处理（如拼接到本地 buffer），最后调用 `CommitRead(len)` 推进 `read_pos`。

镜像映射下使用 `PeekContig(shm, &data, &len)`：永远只返回一段，可读数据即使跨越环尾，在虚拟地址上也是连续的。`RingChannel::Peek` 会按映射类型自动选择，镜像路径下 `seg2` 恒为空。

`CommitRead` 的 `len` 不得超过当前可读字节数：debug 构建触发 `assert`，release 构建钳制到实际可读字节数，避免破坏 `read_pos <= write_pos` 的不变式。

---

## 8. 内存序（Memory Ordering）总结

本缓冲区只有两个共享原子变量，每个只有一个写方，天然满足 SPSC 前提。内存序选择原则：**对自己的变量 relaxed，对对方的变量 acquire/release 同步**。

### 写方视角（TryWrite）

```cpp
w = write_pos.load(relaxed);    // 只有我写它，relaxed 就够
r = read_pos.load(acquire);     // 需要看到读方最新进度
                                // acquire 保证读方之前的 release store 可见
// ... memcpy ...

write_pos.store(w + len, release);  // 发布数据
                                     // release 保证之前的 memcpy 全部对
                                     // 任何 acquire load 此值的人可见
```

### 读方视角（ReadExact / TryRead）

```cpp
r = read_pos.load(relaxed);     // 只有我写它，relaxed 就够
w = write_pos.load(acquire);    // 需要看到写方最新进度
                                // acquire 保证写方的 memcpy 在此之前已完成
// ... CopyOut ...

read_pos.store(r + len, release);  // 释放已消费空间
                                    // release 保证写方下次 acquire load
                                    // read_pos 时能看到一致状态
```

### 同步配对关系

```
写方 write_pos.store(release)  ←→  读方 write_pos.load(acquire)
读方 read_pos.store(release)   ←→  写方 read_pos.load(acquire)
```

每一对 release-store / acquire-load 构成一个**同步点**，保证在 store 之前的所有写操作（包括 memcpy 写入的数据）在 load 之后可见。

### reader_wait 的额外约束

按需通知（第 10 节）在 `write_pos` 与 `reader_wait` 之间需要 store→load 的全序，acquire/release 不够，两端各加一个 `seq_cst` 栅栏：

```
写方: write_pos.store(release) → fence(seq_cst) → reader_wait.load
读方: reader_wait.store(Waiting) → fence(seq_cst) → write_pos.load
```

x86 的 TSO 允许 store→load 重排，所以这两个栅栏会生成真正的屏障指令；其余 acquire/release 操作在 x86 上仍是零额外指令。

---

## 9. BatchWriter 批量写入优化

### 问题：逐条写入的原子操作开销

每次 `TryWrite` 需要 **3 次原子操作**：

1. `write_pos.load(relaxed)` — 读自己位置
2. `read_pos.load(acquire)` — 读对方位置（**最昂贵**：可能触发跨核缓存行迁移）
3. `write_pos.store(release)` — 发布新数据

N 次写入 → 3N 次原子操作。

### 解决方案：快照 + 延迟发布

`BatchWriter` 在构造时一次性快照 `write_pos` 和 `read_pos`，后续写入只操作本地变量，`Flush` 时一次性 store：

```
逐条写入 (N=4):                     批量写入 (N=4):

  load w  load r  memcpy  store w     构造: load w  load r
  load w  load r  memcpy  store w     TryWrite: 本地 w += len, memcpy
  load w  load r  memcpy  store w     TryWrite: 本地 w += len, memcpy
  load w  load r  memcpy  store w     TryWrite: 本地 w += len, memcpy
                                      TryWrite: 本地 w += len, memcpy
  原子操作: 12 次                     Flush:    store w
                                      原子操作: 3 次
```

### read_pos 快照的保守性

构造时读到的 `read_pos` 是当时的值。在批量写入过程中，读方可能继续消费数据（`read_pos` 增大），但 BatchWriter 看不到——它认为空间比实际更少。这是**安全的保守估计**：

- 实际空间 ≥ BatchWriter 认为的空间
- 不会写入超出可用空间的数据
- 代价是可能误报空间不足（`TryWrite` 返回 -1），但不会产生数据错误

### BatchWriter 中的回绕

BatchWriter 的 `TryWrite` 使用完全相同的拆分拷贝逻辑处理回绕，只是操作的是本地 `w_` 而非原子变量。多条数据可能跨越环尾——每次 `TryWrite` 独立计算 `phys_w` 和 `tail`，回绕在数据拷贝时透明处理。构造时传入 `mirrored=true`（`RingChannel::StartBatch` 自动传入）时，跨尾写入也只需一次 `memcpy`。

### Reserve / CommitReserve：直写

```cpp
char *p = batch.Reserve(len);      // 预留 len 字节，返回环内直写指针
if (p) { /* 原地构造数据 */ batch.CommitReserve(len); }
```

调用方直接在环内存上构造数据，省掉一次中间拷贝。空间不足时 `Reserve` 返回 `nullptr`；普通映射下预留区域跨环尾时也返回 `nullptr`（调用方应回退到 `TryWrite`），镜像映射下不会因跨尾而失败。`CommitReserve` 在 release 构建下同样校验"已 Reserve 且长度一致"，不满足则不推进写位置。

### Cancel / RewindTo：回滚未发布的写入

- `Cancel()`：丢弃自上次 `Flush` 以来的所有未发布写入，析构时不再发布
- `RewindTo(pending, count)`：回退到之前记录的 `PendingBytes()/Count()` 状态

对端只能看到 `write_pos` 之前的数据，所以回滚只是把本地 `w_` 退回去，对端无感知。codec 层靠它做异常安全：写帧回调抛异常时回滚，避免向对端发布半截帧。

### 使用约束

- **同一环上同一时刻只允许一个活跃的 BatchWriter**：每个实例都从共享内存快照 `write_pos`，两个并存的实例会写到同一物理位置并互相覆盖
- moved-from 的 BatchWriter 是惰性对象：`TryWrite`/`Reserve` 直接返回失败，不会误写共享环
- 移动赋值会先 `Cancel` 目标对象的未发布数据，不会隐式向对端发布半成品批次

---

## 10. reader_wait：按需通知

读方正在持续消费时，写方每条消息都 `write(eventfd)` 是浪费。`reader_wait` 让写方只在读方**即将睡眠**时才通知一次：

| 取值                        | 含义                                                |
| --------------------------- | --------------------------------------------------- |
| `kReaderAwake = 0`        | 读方处理中，写方无需通知                            |
| `kReaderWaiting = 1`      | 读方即将/已经睡眠，写方需通知一次（通知时清零）     |
| `kReaderNotifyAlways = 2` | `Init` 的初值：读方未启用按需通知，写方每次都通知 |

```
读方 ArmReaderWait(shm, seen_write)      写方 ShouldNotify(shm)
───────────────────────────────          ──────────────────────
                                         write_pos.store(release)  ← 先发布数据
reader_wait.store(Waiting)               fence(seq_cst)
fence(seq_cst)                           v = reader_wait.load
write_pos.load → 与 seen_write 比较       Awake  → 不通知
  变了 → store(Awake)，返回 false         Always → 通知
  没变 → 返回 true，可以睡眠              Waiting→ exchange(Awake)，抢到者通知
```

两端各自"先写、栅栏、再读对方变量"，至少有一方能看到对方的写入，因此**不会丢唤醒**；`exchange` 保证一次睡眠周期只有一个写方路径发通知。

两条规则：

1. `seen_write` 必须在**读环之前**取（`RingBuf::WritePos`）。读完之后才取，期间到达的数据会被算进基线，复查时看不出变化，读方带着未处理的数据睡去
2. `ArmReaderWait` 返回 `false` 表示有新数据，不得睡眠，应继续读取

从未调用过 `ArmReaderWait` 的读方，标志保持 `kReaderNotifyAlways`，行为与引入该机制之前一致。

---

## 11. 镜像映射

`RingChannel` 通过 `MmapRegion::MapMirrored` 把数据区在虚拟地址上**连续映射两圈**，两圈指向同一批物理页：

```
base          base+4096         base+4096+Cap        base+4096+2·Cap
 ├─ header/填充页 ─┤── 数据区（第一圈）──┤── 数据区（第二圈）──┤
                         └──────── 同一批物理页 ────────┘
```

任何起点在 `[0, Capacity)`、长度不超过 `Capacity` 的窗口在 VA 上都是连续的，因此：

- 跨环尾的写入/读取只需**单次 `memcpy`**：`TryWriteContig` / `TryReadContig` / `ReadExactContig`
- `PeekContig` 永远返回单段
- `BatchWriter::Reserve` 不会因跨尾而失败

这组 `*Contig` 方法要求 shm 是镜像映射建立的，普通映射请使用第 3–7 节的两段式方法。镜像只影响本进程的 VA 布局，两端可以一边镜像、一边普通映射；镜像映射失败时 `RingChannel` 自动退化为普通映射。

下面 11.1–11.4 讲原理所需的前置知识，11.5 起讲镜像映射本身。

### 11.1 虚拟地址与物理页

- 程序里的指针是**虚拟地址（VA）**，不是内存条上的真实位置。
- 内核把物理内存切成 4KB 的**页**，用**页表**记录"虚拟页 → 物理页"的对应关系。
- CPU 访问虚拟地址时，硬件（MMU）查页表，换成物理地址再读写。

```
虚拟地址空间               页表                 物理内存
VA 页 0x1000  ──────►  [映射到] ──────►  物理页 #57
VA 页 0x2000  ──────►  [映射到] ──────►  物理页 #12
```

**VA 的连续性与物理页的位置是两回事**：相邻的虚拟页可以对应相隔很远的物理页；反过来，两个不同的虚拟页也可以指向同一个物理页。镜像映射利用的正是后一点。

### 11.2 memfd 与 mmap

- `memfd_create` 创建只存在于内存里的匿名文件，没有磁盘路径。它在内核里是 tmpfs（shmem）上的一个文件，内容由内核分配的物理页保存。
- `ftruncate` 只设置文件**长度**，不分配物理页；页在第一次被访问时才按需分配（见 11.3 的缺页流程）。因此一个 8MB 的环，在被写到之前几乎不占物理内存。
- `mmap(fd, offset, length)` 在进程 VA 空间里划出一段，让这段 VA 对应到文件 `[offset, offset+length)` 所在的物理页。
- 本库所有环映射都使用 `MAP_SHARED`（`ring_channel.hpp`）：写入直接落在文件页上，对所有映射者可见。

```
文件 (memfd):   [页0][页1][页2][页3]       ← 文件页，由内核管理的物理页
                  ▲
mmap(offset=0) ───┘  VA: [A0][A1][A2][A3]
```

### 11.3 页表细节

**四级页表（x86-64，4KB 页）。** 48 位 VA 被拆成五段，前四段各 9 位，依次作为 PGD、PUD、PMD、PTE 四级表的下标，最后 12 位是页内偏移：

```
 47      39 38      30 29      21 20      12 11         0
 ┌─────────┬──────────┬──────────┬──────────┬───────────┐
 │ PGD 下标 │ PUD 下标  │ PMD 下标  │ PTE 下标  │  页内偏移  │
 └────┬────┴────┬─────┴────┬─────┴────┬─────┴───────────┘
      ▼         ▼          ▼          ▼
    PGD 表 →  PUD 表 →   PMD 表 →   PTE 表 → PTE（含物理页号 PFN）
                                                 │
                         物理地址 = PFN × 4096 + 页内偏移
```

每张表占一页，有 512 项，每项 8 字节。最底层的 **PTE** 才指向真正的物理页，其中关键位：

| PTE 内容         | 含义                                            |
| ---------------- | ----------------------------------------------- |
| PFN（物理页号）  | 这一虚拟页对应哪个物理页                        |
| Present          | 该页是否已映射到物理页；为 0 则访问触发缺页异常 |
| R/W              | 是否可写（`PROT_WRITE`）                      |
| U/S              | 用户态是否可访问（`PROT_NONE` 时不可访问）    |
| Accessed / Dirty | 硬件置位，供内核做页面回收与回写                |

**MMU 与 TLB。** 每次访问都走四级页表太慢，所以 CPU 用 **TLB** 缓存最近的"VA 页 → PFN"翻译；TLB 未命中时由硬件遍历页表补齐。

**VMA 与缺页（lazy 填充）。** `mmap` 本身**不**填写 PTE，内核只记一条 **VMA**（`vm_area_struct`）：这段 VA 的范围、权限、对应的文件和文件偏移。PTE 在第一次访问时才由缺页处理填上：

```
1. 进程访问 VA，页表中 Present=0        → CPU 触发缺页异常
2. 内核按 VA 找到所属 VMA               → 得到 (文件, 文件内页号 = VA 偏移 + VMA 的文件偏移)
3. 在该文件的页缓存里按页号查找物理页   → 没有就分配一页（内容清零）并放入页缓存
4. 把这个物理页的 PFN 写进 PTE，置 Present → 返回，CPU 重试指令
```

注意第 3 步：**物理页是按"文件 + 页号"查找的，而不是按 VA。** 这是下一节"物理上只有一份"的根源。

### 11.4 为什么物理上只占一份内存

同一个 memfd 映射两次，得到两个不同的 VMA、两组不同的 PTE，但它们都经由"文件 + 页号"去页缓存里取页：

```
文件页号:           0     1     2     3
页缓存里的物理页:   [P0]  [P1]  [P2]  [P3]      ← 每个页号只有一个物理页

VA 区域 X (第一次映射):  [X0]  [X1]  [X2]  [X3]      PTE 的 PFN 分别 = P0 P1 P2 P3
VA 区域 Y (第二次映射):  [Y0]  [Y1]  [Y2]  [Y3]      PTE 的 PFN 分别 = P0 P1 P2 P3
```

- X 与 Y 各自有一套 PTE（映射表项各占 8 字节），**但物理页只有一份**。多出来的开销只是页表项，不是数据拷贝。
- 因为是 `MAP_SHARED`，写入直接修改页缓存里的物理页，没有写时复制。如果误用 `MAP_PRIVATE`，写入时内核会为该映射复制出私有页，两个别名就不再同步，镜像会失效。
- **CPU 缓存也不会出现两份不一致的数据。** x86-64 的缓存按物理地址（或等价于物理地址）定位缓存行，X0 与 Y0 翻译成同一物理地址，命中同一个缓存行，硬件一致性协议照常工作，不需要额外同步。
- 因此：向 X 区域写 `X0[5] = 'a'`，从 Y 区域读 `Y0[5]` 立刻得到 `'a'`。

这与跨进程共享内存是同一个机制：两个进程各自 `mmap` 同一个 memfd，各自的 VA 不同，PTE 也各自独立，但指向同一批物理页。镜像映射只是把"第二个进程"换成"同一进程里的另一段 VA"。

### 11.5 回绕问题

环写到末尾后绕回开头。普通映射下，跨环尾的数据在 VA 上被劈成两段：

```
容量 = 8，写位置在 6，再写 4 字节：

下标:   0   1   2   3   4   5   6   7
       [c] [d] [ ] [ ] [ ] [ ] [a] [b]
        ▲                       ▲
        └── 后 2 字节绕回开头     └── 前 2 字节在末尾
```

- 写入要拆成两次 `memcpy`；
- 读取要么拆成两次，要么拼到临时缓冲区，多一次拷贝；
- 无法把这段数据当作一块连续内存零拷贝地交给上层。

### 11.6 镜像映射原理

把同一个环映射两遍，让第二遍的 VA **紧接**第一遍：

```
物理页（环本体）:       [   环 8 格   ]
                          ▲        ▲
VA:   [ 第一圈 8 格 ][ 第二圈 8 格 ]    ← 两圈各有一套 PTE，指向同一批物理页
下标:  0 1 2 3 4 5 6 7  0 1 2 3 4 5 6 7
```

仍然从位置 6 写 4 字节：

```
VA:   0 1 2 3 4 5 [6 7  0 1] 2 3 4 5 6 7
                   └─连续 4 字节─┘
```

- 在 VA 上，位置 6–9 是连续的，一次 `memcpy` 写完；
- `[8,10)` 是第二圈的 `[0,2)`，与第一圈的 `[0,2)` 是同一块物理内存，数据实际落在物理环的 `[6,8)` 与 `[0,2)`，对环而言仍是正确的回绕；
- 读取同理：从位置 6 读 4 字节得到一个连续指针，这就是 `PeekContig` 永远返回单段的原因；
- 约束：窗口长度不得超过 `Capacity`（否则会读到第二圈之外），这与"单条消息最大 Capacity/2"的限制一致。

### 11.7 MapMirrored 的实现步骤

文件布局为 `[header 页 (4KB)][数据区 (Capacity)]`，`file_size = data_offset + capacity`。

```
VA:   base                    base+file_size                 base+file_size+capacity
       ├─ header ─┤── 数据 (第一圈) ──┤──── 数据 (第二圈) ────┤
文件:   [0, 4096)   [4096, file_size)     [4096, file_size)  ← 第二圈再次映射数据区
```

1. **参数校验**：`data_offset + capacity == file_size`；`data_offset` 必须是页大小整数倍（`mmap` 的文件偏移要求页对齐，第二圈正是从 `data_offset` 开始映射）。
2. **预留 VA**：`mmap(nullptr, file_size + capacity, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS)` 先占住一段连续地址。若直接连续调用两次 `mmap(nullptr, …)`，内核不保证两块地址相邻。
3. **第一圈**：`mmap(base, file_size, …, MAP_FIXED, fd, 0)`，映射 header 加数据区，覆盖预留区的前 `file_size` 字节。
4. **第二圈**：`mmap(base + file_size, capacity, …, MAP_FIXED, fd, data_offset)`，再次映射同一段数据区，紧接第一圈末尾。
5. **失败回滚**：第 3、4 步失败时先保存 `errno`，再 `munmap(base, 整块)` 回收预留区（`munmap` 可能改写 `errno`），不泄漏 VA。
6. **提交**：两圈都成功后才释放对象原有映射并更新成员；`size_` 记整块预留长度，析构时一次 `munmap` 全部回收。

此时内核里有两个 VMA：`[base, base+file_size)` 对应文件偏移 0，`[base+file_size, +capacity)` 对应文件偏移 `data_offset`，二者的文件偏移不连续，不会被合并。

### 11.8 代价与注意事项

| 项       | 说明                                                                                                                 |
| -------- | -------------------------------------------------------------------------------------------------------------------- |
| 物理内存 | 不增加；仍是一份环 + 一页 header                                                                                     |
| 页表开销 | 每多映射一圈，多 `Capacity/4096` 个 PTE（8 字节/个）。默认 8MB 环为 2048 个，约 16KB                               |
| TLB      | 两圈占用不同的 TLB 表项，跨环尾访问时可能多一次 TLB 未命中，相对省下的一次拷贝可以忽略                               |
| RSS 统计 | 同一物理页经两段 VA 被触碰，`/proc/<pid>/status` 的 RSS 可能高估（约两倍）；看 PSS 或系统 `Shmem` 更接近真实占用 |
| 容量对齐 | `capacity` 必须是页大小整数倍，否则第一圈末页按页取整后会与第二圈重叠；`Capacity` 为 2 的幂且远大于 4KB，满足    |
| 仅本进程 | 镜像只影响本进程 VA 布局，不改变共享的文件内容；对端可以仍用普通映射                                                 |
| 退化     | `MapMirrored` 失败（如 VA 空间不足）时 `RingChannel` 退回普通 `Map`，用两段式方法                              |

---

## 12. API 一览

| 方法                                                         | 签名                                                          | 说明                                                                                  |
| ------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `Init`                                                     | `void Init(void *shm)`                                      | 原地构造 RingHeader 并清零；初值 `reader_wait = kReaderNotifyAlways`；要求 64B 对齐 |
| `TryWrite`                                                 | `int TryWrite(void *shm, const void *data, uint32_t len)`   | 写入 len 字节，0/-1                                                                   |
| `TryRead`                                                  | `uint32_t TryRead(void *shm, void *data, uint32_t max_len)` | 尽力读，返回实际字节数                                                                |
| `ReadExact`                                                | `int ReadExact(void *shm, void *data, uint32_t len)`        | 精确读 len 字节或不读，0/-1                                                           |
| `Peek`                                                     | `int Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len)`         | 两段式零拷贝，0/-1                                                                    |
| `CommitRead`                                               | `void CommitRead(void *shm, uint32_t len)`                  | 推进 read_pos（越界时 debug assert、release 钳制）                                    |
| `TryWriteContig` / `TryReadContig` / `ReadExactContig` | 同上三者                                                      | 镜像映射专用，跨尾单次 memcpy                                                         |
| `PeekContig`                                               | `int PeekContig(shm, &data, &len)`                          | 镜像映射专用，永远单段                                                                |
| `Available`                                                | `uint64_t Available(const void *shm)`                       | 可读字节数                                                                            |
| `FreeSpace`                                                | `uint64_t FreeSpace(const void *shm)`                       | 可写字节数                                                                            |
| `WritePos` / `ReadPos`                                   | `uint64_t WritePos(const void *shm)`                        | 当前位置（`WritePos` 用于取 `seen_write` 基线）                                   |
| `ArmReaderWait`                                            | `bool ArmReaderWait(void *shm, uint64_t seen_write)`        | 读方睡眠前声明，false 表示有新数据                                                    |
| `ShouldNotify`                                             | `bool ShouldNotify(void *shm)`                              | 写方发布后调用，判断是否需写 eventfd                                                  |
| `BatchWriter`                                              | RAII class                                                    | 批量写入，Flush 时发布；支持 Reserve/CommitReserve、Cancel、RewindTo                  |
