# RingBuf 字节流环形缓冲区实现详解

本文档面向初次接触本项目的开发者，详细讲解 `RingBuf` 纯字节流 SPSC 环形缓冲区的实现原理，涵盖共享内存布局、位置管理、写入与读取流程、回绕（wrap-around）机制、内存序语义以及批量写入优化。

---

## 1. 共享内存布局

```
共享内存 (shm_size = 128 + Capacity 字节)
┌──────────────────────────────────────────────────────────────┐
│                  RingHeader (128 bytes)                       │
│  ┌───────────────────────────┐ ┌───────────────────────────┐ │
│  │ write_pos  atomic<u64>    │ │ read_pos   atomic<u64>    │ │
│  │ + 56B padding             │ │ + 56B padding             │ │
│  │ (独占一个 64B 缓存行)     │ │ (独占一个 64B 缓存行)     │ │
│  └───────────────────────────┘ └───────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│                Data Region (Capacity bytes)                   │
│  偏移 0                                          偏移 Cap-1  │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │ b b b b b b b b b b b b b b b ··· b b b b b b b b b b b│ │
│  └──────────────────────────────────────────────────────────┘ │
│  ↑                                                 ↑         │
│  phys_r (读位置)                            phys_w (写位置)   │
└──────────────────────────────────────────────────────────────┘
```

### 两个核心字段

| 字段 | 所在缓存行 | 写方 | 读方 |
|------|-----------|------|------|
| `write_pos` | 第 1 缓存行 (偏移 0) | 写入数据后 **store** | 检测新数据时 **load** |
| `read_pos` | 第 2 缓存行 (偏移 64) | 检测空间时 **load** | 消费数据后 **store** |

两个字段各占独立缓存行（`alignas(64)` + 56 字节 padding），避免 **false sharing**——写方修改 `write_pos` 不会使读方的 `read_pos` 所在缓存行失效，反之亦然。

---

## 2. 位置管理：逻辑偏移与物理偏移

### 单调递增的逻辑偏移

`write_pos` 和 `read_pos` 都是 **单调递增的 64 位字节偏移**，永远不回绕归零。例如一个 Capacity=4096 的环，写入 5000 字节后 `write_pos=5000`，不是 `5000 % 4096 = 904`。

这样设计的好处：
- **已用空间** = `write_pos - read_pos`，一次减法即可，无需处理回绕
- **空闲空间** = `Capacity - (write_pos - read_pos)`
- 64 位永不溢出（以 10GB/s 持续写入也要 58 年才溢出）

### Mask：逻辑偏移 → 物理偏移

```cpp
static constexpr std::size_t Mask(uint64_t pos)
{
    return static_cast<std::size_t>(pos & (Capacity - 1));
}
```

因为 Capacity 必须为 2 的幂，`pos & (Capacity - 1)` 等价于 `pos % Capacity`，但只需一条 AND 指令。

示例（Capacity = 4096 = 0x1000）：
```
pos = 5000  →  Mask(5000) = 5000 & 0xFFF = 904   ← 物理偏移
pos = 4096  →  Mask(4096) = 4096 & 0xFFF = 0      ← 刚好回到起点
pos = 8192  →  Mask(8192) = 8192 & 0xFFF = 0      ← 第二圈起点
```

### 可用空间检查

```
已用 = w - r               （单调递增，直接相减）
可用容量 = Capacity - (w - r)
写入条件: (w + len) - r <= Capacity   即 已用 + 新增 <= 总容量
```

`max_write_size = Capacity / 2` 限制单次最大写入为容量的一半。这简化了空间管理——无论读写位置如何，一次 `max_write_size` 的写入只要空闲超过一半就一定能成功。

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
    ┌─ len == 0 || len > max_write_size? ──── 是 ──→ return -1
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
if (len == 0 || len > max_write_size)
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
std::size_t phys_w = Mask(w);
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
    CopyOut(base, Mask(r), data, len)      ← 处理回绕拷贝
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

| | ReadExact | TryRead |
|---|-----------|---------|
| 语义 | 要么读到恰好 `len` 字节，要么什么都不读 | 读最多 `max_len` 字节，有多少读多少 |
| 返回值 | 0 成功 / -1 失败 | 实际读取的字节数（0 表示空） |
| 不足时行为 | 不消费任何字节 | 读取全部可用字节（可能 < max_len） |
| 适用场景 | codec 层读定长帧 | 原始字节流消费 |

```
to_read = min(avail, max_len)
CopyOut(base, Mask(r), dst, to_read)   ← 同样的回绕处理
read_pos.store(r + to_read, release)
return to_read
```

---

## 6. 回绕（wrap-around）机制详解

回绕是环形缓冲区的核心难点。本实现采用**拆分拷贝**（split-copy），无需哨兵标记。

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

| 物理布局 | 是否会触发回绕 | 安全性原因 |
|---------|--------------|-----------|
| `phys_w >= phys_r` | 可能会 | 空间检查保证 `len - tail <= phys_r`，第二段写不到未读区域 |
| `phys_w < phys_r` | 不会 | 空闲空间在中间一整块，`len <= tail` 恒成立，只走单段 memcpy |

因此，只要空间检查 `(w + len) - r <= Capacity` 通过，回绕写入就绝不会覆盖未读数据。

### 6.6 边界条件

| 条件 | 行为 |
|------|------|
| `len == tail` | 刚好填满尾部，不回绕（走 `len <= tail` 分支） |
| `len < tail` | 完全在尾部空间内，不回绕 |
| `tail == 0` (`phys_w == 0`) | 整个写入在环头部，`len <= Capacity` 走不回绕分支 |
| `len == Capacity/2` | 单次最大写入，最多占半个环 |

---

## 7. Peek + CommitRead 两段式零拷贝

当可读数据跨越环尾时，无法用单个指针表示。`Peek` 返回两段：

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

BatchWriter 的 `TryWrite` 使用完全相同的拆分拷贝逻辑处理回绕，只是操作的是本地 `w_` 而非原子变量。多条数据可能跨越环尾——每次 `TryWrite` 独立计算 `phys_w` 和 `tail`，回绕在数据拷贝时透明处理。

---

## 10. API 一览

| 方法 | 签名 | 说明 |
|------|------|------|
| `Init` | `void Init(void *shm)` | 清零初始化 shm |
| `TryWrite` | `int TryWrite(void *shm, const void *data, uint32_t len)` | 写入 len 字节，0/-1 |
| `TryRead` | `uint32_t TryRead(void *shm, void *data, uint32_t max_len)` | 尽力读，返回实际字节数 |
| `ReadExact` | `int ReadExact(void *shm, void *data, uint32_t len)` | 精确读 len 字节或不读，0/-1 |
| `Peek` | `int Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len)` | 两段式零拷贝，0/-1 |
| `CommitRead` | `void CommitRead(void *shm, uint32_t len)` | 推进 read_pos |
| `Available` | `uint64_t Available(const void *shm)` | 可读字节数 |
| `FreeSpace` | `uint64_t FreeSpace(const void *shm)` | 可写字节数 |
| `BatchWriter` | RAII class | 批量写入，Flush 时发布 |
