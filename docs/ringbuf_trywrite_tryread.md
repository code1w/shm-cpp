# RingBuf TryWrite / TryRead 详解

本文档面向初次接触本项目的开发者，逐行解释 `RingBuf` 两个核心函数的工作原理。

---

## 前置知识

### 共享内存布局

```
共享内存 (shm_size 字节)
┌──────────────────────────────────────────────────────────────┐
│                  RingHeader (128 bytes)                       │
│  ┌───────────────────────────┐ ┌───────────────────────────┐ │
│  │ write_pos  atomic<u64>    │ │ read_pos   atomic<u64>    │ │
│  │ + 56B padding             │ │ + 56B padding             │ │
│  │ (独占一个 64B 缓存行)     │ │ (独占一个 64B 缓存行)     │ │
│  └───────────────────────────┘ └───────────────────────────┘ │
├──────────────────────────────────────────────────────────────┤
│            Data Region (Capacity bytes)                       │
│  偏移 0                                          偏移 Cap-1  │
│  ┌──────┬────────┬───┬──────┬────────┬───┬─────────────────┐ │
│  │MsgHdr│payload │pad│MsgHdr│payload │pad│   空闲空间 ...  │ │
│  └──────┴────────┴───┴──────┴────────┴───┴─────────────────┘ │
│  ↑                          ↑                      ↑         │
│  phys_r (读位置)        某条消息             phys_w (写位置)  │
└──────────────────────────────────────────────────────────────┘
```

### 关键概念

| 概念 | 说明 |
|------|------|
| `write_pos` / `read_pos` | **单调递增**的逻辑字节偏移，永远不回绕归零。已写入的总字节数 - 已读取的总字节数 = 环中数据量 |
| `Mask(pos)` | `pos & (Capacity - 1)`，将逻辑偏移转换为物理偏移（0 ~ Capacity-1）。等价于取模但只需一条 AND 指令 |
| `FrameSize(len)` | `(8 + len + 7) & ~7`，一条消息在环中占用的实际字节数（8B 头 + payload + 对齐填充） |
| 哨兵 (Sentinel) | `MsgHeader.len = UINT32_MAX`，标记"此位置到环尾的空间作废，请从偏移 0 继续读" |

### 消息帧格式

```
┌──────────┬──────────┬─────────────────────┬────────────┐
│ len (4B) │ seq (4B) │ payload (len bytes) │ pad to 8B  │
└──────────┴──────────┴─────────────────────┴────────────┘
 ◀── MsgHeader (8B) ──▶
 ◀──────────────── FrameSize(len) 字节 ──────────────────▶
```

---

## TryWrite 逐行解析

```cpp
static int TryWrite(void *shm, const void *data, uint32_t len, uint32_t seq)
```

**功能**: 尝试将一条消息写入环形缓冲区。空间不足时返回 -1（不阻塞）。

### 第一步：前置校验

```cpp
if (len > max_msg_size)                          // max_msg_size = Capacity/2 - 8
    return -1;
```

消息过大直接拒绝。上限为 Capacity/2 是为了保证 write_pos 永远不会"套圈" read_pos（逻辑差值不超过 Capacity）。

### 第二步：读取当前位置

```cpp
auto *hdr  = Header(shm);                        // shm 起始处就是 RingHeader
char *base = DataRegion(shm);                     // 数据区 = shm + sizeof(RingHeader)

uint64_t w         = hdr->write_pos.load(relaxed);  // ① 读自己的写位置
uint64_t r         = hdr->read_pos.load(acquire);   // ② 读对方的读位置
std::size_t total  = FrameSize(len);                 // 本帧占用字节数
std::size_t phys_w = Mask(w);                        // 写位置的物理偏移
std::size_t tail   = Capacity - phys_w;              // 从写位置到环尾的剩余连续空间
```

**为什么 write_pos 用 relaxed？** 只有写方修改 write_pos，没有竞争，relaxed 就够了。

**为什么 read_pos 用 acquire？** 需要看到读方最新释放的空间。读方通过 `release` store 推进 read_pos，这里的 `acquire` 与之配对，保证：如果我看到 read_pos=X，那么读方在 store X 之前完成的所有操作（包括读取旧数据的 memcpy）对我可见——我可以安全地覆写那些位置。

### 第三步：判断是否需要回绕

分两种情况——帧能放下 vs 放不下（需要哨兵回绕）：

```
情况 A: total <= tail — 帧能放在当前位置
┌─────┬──────────────┬──────────────────┬──────────┐
│ ... │ [已有消息]   │ phys_w ──写入──→ │  空闲    │
└─────┴──────────────┴──────────────────┴──────────┘
                      ◀── tail 足够放 total ──▶

情况 B: total > tail — 尾部放不下，需要哨兵回绕
┌────────────────────┬──────────────┬──────┐
│ 从偏移0写入正文 ←  │ [已有消息]   │ 尾部 │
│                    │              │ 不够 │
└────────────────────┴──────────────┴──────┘
  ◀── total ──▶                     ◀tail▶
                                    在这里写哨兵
```

### 第四步 情况 B：哨兵回绕路径

```cpp
if (total > tail)
{
    // 空间检查：需要 tail（哨兵占位）+ total（正文）
    if ((w + tail + total) - r > Capacity)         // ③
        return -1;                                  // 放不下，返回失败
```

**③ 空间公式解释**: `w - r` 是当前环中已使用的字节数。写入后会变成 `(w + tail + total) - r`。如果超过 Capacity，说明写指针会追上读指针——缓冲区溢出。

```cpp
    // 写哨兵：告诉读方"这里到环尾的空间作废"
    auto *sentinel = reinterpret_cast<MsgHeader *>(base + phys_w);
    sentinel->len  = kSentinel;                     // UINT32_MAX = 哨兵标记
    sentinel->seq  = 0;
    w += tail;                                      // 逻辑写位置跳过整个尾部
```

```
写入哨兵后：
              phys_w
                ↓
┌──────────────[SENTINEL xxxxxxxxxx]┐
│  空闲空间                         │
└───────────────────────────────────┘
  ↑
  偏移 0，正文将写在这里
```

```cpp
    // 在偏移 0 写入正文
    auto *mh = reinterpret_cast<MsgHeader *>(base); // base + 0
    mh->len  = len;
    mh->seq  = seq;
    std::memcpy(base + msg_header_size, data, len); // 拷贝 payload

    hdr->write_pos.store(w + total, release);       // ④ 发布新的 write_pos
}
```

**④ 为什么用 release？** 保证上面所有的 memcpy / 帧头写入在 write_pos 更新之前对读方可见。读方用 `acquire` 读 write_pos，看到新值时，数据一定已经就绪。

### 第五步 情况 A：正常路径（无回绕）

```cpp
else
{
    if ((w + total) - r > Capacity)                 // 空间检查
        return -1;

    auto *mh = reinterpret_cast<MsgHeader *>(base + phys_w);
    mh->len  = len;
    mh->seq  = seq;
    std::memcpy(base + phys_w + msg_header_size, data, len);

    hdr->write_pos.store(w + total, release);       // 发布
}
return 0;
```

逻辑与回绕路径相同，只是直接在 `phys_w` 位置写入，无需哨兵。

### TryWrite 全流程图

```
TryWrite(data, len=100, seq=1)
│
├─ len > max_msg_size? → 是 → return -1
│
├─ 读取 w=2048, r=1024
│  total = FrameSize(100) = (8+100+7)&~7 = 112
│  phys_w = 2048 & (Cap-1) = 2048
│  tail = Cap - 2048
│
├─ total > tail?
│  │
│  ├─ 是 (回绕)
│  │  ├─ (w + tail + total) - r > Cap? → 是 → return -1
│  │  ├─ 在 phys_w 写 SENTINEL
│  │  ├─ w += tail
│  │  ├─ 在偏移 0 写 [MsgHdr][payload]
│  │  └─ write_pos.store(w + total, release)
│  │
│  └─ 否 (正常)
│     ├─ (w + total) - r > Cap? → 是 → return -1
│     ├─ 在 phys_w 写 [MsgHdr][payload]
│     └─ write_pos.store(w + total, release)
│
└─ return 0
```

---

## TryRead 逐行解析

```cpp
static int TryRead(void *shm, void *data, uint32_t *len, uint32_t *seq)
```

**功能**: 尝试从环形缓冲区读取一条消息到用户缓冲区。环为空时返回 -1。

### 第一步：读取当前位置

```cpp
auto *hdr        = Header(shm);
const char *base = DataRegion(shm);

uint64_t r = hdr->read_pos.load(relaxed);         // ① 读自己的读位置
uint64_t w = hdr->write_pos.load(acquire);         // ② 读对方的写位置

if (r >= w)
    return -1;                                      // 环为空
```

**① relaxed**: 只有读方修改 read_pos，无竞争。

**② acquire**: 需要看到写方 release store 之前写入的所有数据（帧头 + payload 的 memcpy）。这是正确性的关键——如果没有 acquire，可能看到 write_pos 已更新但 payload 数据还未刷出 store buffer，读到脏数据。

**`r >= w` 判定为空**: 因为 write_pos 和 read_pos 都是单调递增的，`w > r` 意味着有未读数据，`w == r` 意味着所有数据都已读完。

### 第二步：定位消息帧

```cpp
std::size_t phys_r = Mask(r);                       // 读位置的物理偏移
auto *mh = reinterpret_cast<const MsgHeader *>(base + phys_r);
```

直接在数据区的 `phys_r` 偏移处解读帧头。

### 第三步：处理哨兵

```cpp
if (mh->len == kSentinel)                           // 遇到哨兵？
{
    r += Capacity - phys_r;                          // 逻辑读位置跳到下一个 Capacity 对齐点
    if (r >= w)                                      // 跳过后可能环就空了
        return -1;

    phys_r = Mask(r);                                // 重新计算物理偏移（此时 = 0）
    mh     = reinterpret_cast<const MsgHeader *>(base + phys_r);
}
```

```
遇到哨兵时的内存状态：
                     phys_r
                       ↓
┌──────────────────[SENTINEL xxxxxx]┐
│ [MsgHdr][payload][pad] ...        │
└───────────────────────────────────┘
  ↑
  跳到这里继续读（偏移 0）

跳过后：
  phys_r = 0
    ↓
┌──[MsgHdr][payload][pad] ...       ┐
│                       [SENTINEL]  │
└───────────────────────────────────┘
```

**`r += Capacity - phys_r` 的含义**: 逻辑上跳过从当前物理位置到环尾的所有字节。由于 Capacity 是 2 的幂，`Mask(r)` 之后新的物理偏移恰好为 0。

### 第四步：读取消息

```cpp
uint32_t payload_len = mh->len;
if (payload_len > max_msg_size)                      // 防御性检查
    return -1;
if (seq)
    *seq = mh->seq;                                  // 输出序列号
if (len)
    *len = payload_len;                              // 输出 payload 长度
if (data)
    std::memcpy(data, base + phys_r + msg_header_size, payload_len);  // ③ 拷贝
```

**③ 数据拷贝**: 从环形缓冲区的 `phys_r + 8`（跳过 MsgHeader）开始，拷贝 `payload_len` 字节到用户提供的缓冲区。这是整个读取路径中唯一的 memcpy。

### 第五步：推进读位置

```cpp
hdr->read_pos.store(r + FrameSize(payload_len), release);  // ④
return 0;
```

**④ release store 的含义**: 告诉写方"我已经读完了这些数据，你可以覆写这些位置了"。写方通过 `acquire` 读 read_pos，看到新值后就知道空间已释放。

### TryRead 全流程图

```
TryRead(data, &len, &seq)
│
├─ 读取 r=1024, w=2048
│  r >= w? → 否（有数据）
│
├─ phys_r = 1024 & (Cap-1)
│  读取 base[phys_r] 处的 MsgHeader
│
├─ mh->len == SENTINEL?
│  │
│  ├─ 是
│  │  ├─ r += Cap - phys_r  (跳到环头)
│  │  ├─ r >= w? → 是 → return -1
│  │  └─ phys_r = Mask(r) = 0, 重新读 MsgHeader
│  │
│  └─ 否 (正常消息)
│
├─ payload_len > max_msg_size? → 是 → return -1
│
├─ 输出 seq, len
├─ memcpy(data, base + phys_r + 8, payload_len)
├─ read_pos.store(r + FrameSize(payload_len), release)
│
└─ return 0
```

---

## 写入-读取配对示例

以 Capacity=32 为例，展示 3 条消息的写入与读取过程：

```
初始状态: write_pos=0, read_pos=0
Data Region (32 bytes):
[________][________][________][________]
 0         8         16        24

═══ TryWrite("Hi", len=2, seq=1) ═══
FrameSize(2) = (8+2+7)&~7 = 16
phys_w=0, tail=32, total=16 <= tail → 正常路径
空间: (0+16)-0=16 <= 32 ✓

[len=2|s=1][Hi|pad..][________][________]
 0         8         16        24
write_pos = 0 + 16 = 16

═══ TryWrite("World!", len=6, seq=2) ═══
FrameSize(6) = (8+6+7)&~7 = 16
phys_w=16, tail=16, total=16 <= tail → 正常路径
空间: (16+16)-0=32 <= 32 ✓

[len=2|s=1][Hi|pad..][len=6|s=2][World!|p]
 0         8         16         24
write_pos = 16 + 16 = 32

═══ TryRead → 读取第 1 条 ═══
r=0, w=32, r < w → 有数据
phys_r=0, mh->len=2 (非哨兵)
memcpy → "Hi"
read_pos = 0 + 16 = 16

═══ TryWrite("AB", len=2, seq=3) ═══
phys_w = Mask(32) = 0   ← write_pos=32 回到物理偏移 0
tail = 32 - 0 = 32, total=16 <= tail → 正常路径
空间: (32+16)-16=32 <= 32 ✓

[len=2|s=3][AB|pad..][len=6|s=2][World!|p]
 0          8        16         24
 ↑ 覆写了已读的第1条           (第2条仍在)
write_pos = 48

═══ TryRead → 读取第 2 条 ═══
r=16, w=48
phys_r=16, mh->len=6 → "World!"
read_pos = 16 + 16 = 32

═══ TryRead → 读取第 3 条 ═══
r=32, w=48
phys_r = Mask(32) = 0, mh->len=2 → "AB"
read_pos = 32 + 16 = 48
```

---

## 哨兵回绕示例

```
状态: write_pos=24, read_pos=16, Capacity=32

[________][________][len=6|s=2][World!|p]
 0         8         16         24
                     ↑ read      ↑ write

═══ TryWrite("Hello!", len=6, seq=4) ═══
FrameSize(6) = 16
phys_w = 24, tail = 32-24 = 8
total=16 > tail=8 → 需要哨兵回绕!

空间检查: (24 + 8 + 16) - 16 = 32 <= 32 ✓

① 在 phys_w=24 写入哨兵:
[________][________][len=6|s=2][SENTINEL]
                                ↑

② w += tail → w = 32

③ 在偏移 0 写入正文:
[len=6|s=4][Hello!|p][len=6|s=2][SENTINEL]
 ↑ 正文                          ↑ 哨兵

write_pos = 32 + 16 = 48

═══ TryRead ═══
r=16, w=48, phys_r=16
mh->len=6 → 正常消息 "World!" → read_pos=32

═══ TryRead ═══
r=32, w=48, phys_r = Mask(32) = 0
mh->len=6 → 正常消息 "Hello!" → read_pos=48
   等等，phys_r=0 处直接就是正文，因为上一步读完后 r=32,
   Mask(32)=0，而偏移 0 处是 len=6 不是 SENTINEL，直接读。

但如果上一步 read_pos 停在 24 呢？
r=24, phys_r=24, mh->len=SENTINEL!
→ r += 32-24 = 8 → r=32, phys_r=Mask(32)=0
→ 读偏移 0 处的 "Hello!"
```

---

## 内存序总结图

```
写方 (TryWrite)                       读方 (TryRead)
═══════════════                      ═══════════════

load write_pos [relaxed] ①           load read_pos [relaxed] ④
  ↓ (自己独占，无竞争)                   ↓ (自己独占，无竞争)

load read_pos [acquire] ②            load write_pos [acquire] ⑤
  ↓ 与 ⑥ 同步：                        ↓ 与 ③ 同步：
  │ 看到读方释放的空间                    │ 看到写方写入的数据
  │                                      │
写入 MsgHeader + memcpy                 读取 MsgHeader + memcpy
  ↓                                      ↓

store write_pos [release] ③           store read_pos [release] ⑥
  ↓ 发布数据给读方                       ↓ 释放空间给写方
  └───── ⑤ acquire 读到 ③ ──────→       └──── ② acquire 读到 ⑥ ────→
```

②↔⑥ 配对：写方看到读方释放的空间后，才覆写那些位置。
③↔⑤ 配对：读方看到写方更新的 write_pos 时，payload 数据一定已完整写入。

---

## write_pos / read_pos 溢出分析

### 设计：单调递增，永不归零

`write_pos` 和 `read_pos` 都是 `uint64_t`，**单调递增，永远不主动回绕到 0**。每次写入推进 `write_pos += FrameSize(len)`，每次读取推进 `read_pos += FrameSize(len)`，它们记录的是"从创建至今累计经过的总字节数"，而非环中的物理位置。

物理定位完全依赖位掩码：

```cpp
static constexpr std::size_t Mask(uint64_t pos)
{
    return static_cast<std::size_t>(pos & (Capacity - 1));
}
```

无论 `pos` 多大，`pos & (Capacity - 1)` 的结果始终在 `[0, Capacity)` 范围内。

### uint64 实际上不可能溢出

`uint64_t` 最大值为 `2^64 - 1 = 18,446,744,073,709,551,615`。假设以极端速率持续写入：

```
写入速率          耗尽时间
──────────────────────────
1 GB/s            约 585 年
10 GB/s           约 58.5 年
100 GB/s          约 5.8 年
```

即使是 100 GB/s 的极端吞吐也需要近 6 年不间断运行才会溢出。在任何实际工程场景中，**可以认为永远不会溢出**。

### 即使自然溢出，算术仍然正确

假设真的发生了 `uint64_t` 回绕，所有关键运算依然安全。原因是 C++ 标准明确规定无符号整数的溢出行为是模 `2^N` 运算（非未定义行为）。

**空间检查公式**：

```cpp
// TryWrite 中判断空间是否足够
(w + total) - r > Capacity
```

只要 `w` 与 `r` 的**实际差值**不超过 `2^64`——这不可能，因为差值就是环中的数据量，最大也就 Capacity（几十 MB）——无符号减法 `w - r` 的结果就是正确的已用字节数，即使 `w` 在数值上小于 `r`（发生了回绕）。

举例：

```
假设 uint64 回绕发生：
  w = 5               (回绕后的小数值)
  r = 0xFFFFFFFF...FA (回绕前的大数值)

  w - r = 5 - 0xFFFFFFFF...FA
        = 11           (模 2^64 运算，结果正确：环中有 11 字节数据)
```

**位掩码寻址**：

```
write_pos = 0xFFFFFFFFFFFFFFFF (uint64 最大值)
Capacity  = 0x800000           (8MB)

Mask = 0xFFFFFFFFFFFFFFFF & 0x7FFFFF = 0x7FFFFF = 8388607
→ 合法的物理偏移，完全正确
```

### 对比"归零回绕"设计

传统环形缓冲区常用"到顶归零"的方式管理位置，即 `write_pos` 在到达 Capacity 时重置为 0：

```
归零回绕设计（本项目未采用）:
  write_pos ∈ [0, Capacity)
  已用空间 = (write_pos >= read_pos)
             ? write_pos - read_pos
             : Capacity - read_pos + write_pos    ← 需要分支判断
```

```
单调递增设计（本项目采用）:
  write_pos ∈ [0, 2^64)
  已用空间 = write_pos - read_pos                  ← 始终正确，无分支
  物理偏移 = pos & (Capacity - 1)                   ← 一条 AND 指令
```

单调递增设计的优势：
- **无分支**：`w - r` 直接得到已用字节数，不需要考虑 `w < r` 的情况
- **无额外状态**：不需要"满/空"标志位来区分 `w == r` 是满还是空（单调递增下 `w == r` 一定是空）
- **原子操作更简单**：不需要同时原子地更新位置和状态标志
