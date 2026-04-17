# ProtoCodec 跨环尾回绕测试原理

## 测试目标

验证 protobuf 消息在 ring buffer 跨环尾回绕时能否正常编码和解码。

## 为什么需要回绕

ring buffer 是一段固定大小的连续内存，write_pos 单调递增，物理位置通过位掩码映射：

```
physical_offset = write_pos & (Capacity - 1)
```

当一帧数据写到环尾时，物理空间不够放下整帧，需要拆成两段：前半部分写到环尾，后半部分回绕到环头。

```
write_pos = 4090, 帧大小 = 42 字节

                    4090     4095  0        31
                      │        │  │         │
环内存: ─────────────[前6字节]──┤──[后36字节]──────────
                      ▲  环尾拆分点
```

帧的任何部位都可能落在拆分点上：MsgHeader、type_name_len、type_name、pb_data。

## 如何触发回绕

使用 4096 字节的极小环。每帧大小：

| 帧类型 | 组成 | 大小 |
|--------|------|------|
| HeartbeatProto | MsgHeader(8) + name_len(2) + "shm_ipc.HeartbeatProto"(22) + pb(~10) | ~42B |
| ClientMsgProto(200B payload) | MsgHeader(8) + name_len(2) + "shm_ipc.ClientMsgProto"(22) + pb(~210) | ~242B |

一对（偶+奇）约 284 字节，30 帧累计约 4260 字节，超过环容量。write_pos 会多次越过 4096 边界，不同帧的不同部位自然落在拆分点上。

## 写入流程

子进程交替发送两种大小的 protobuf 消息：

```
for i in 0..29:
    构造消息（偶数帧 HeartbeatProto，奇数帧 ClientMsgProto）
    while Send(ch, msg, seq) != 0:   ← 环满时 spin 等待读端消费
        ;
```

Send 内部调用链：

```
ProtoCodec<T>::Send(ch, msg, seq)
  └─ SendFrame(ch, payload_len, seq, write_payload_fn)
       ├─ BatchWriter::TryWrite(MsgHeader)       ← 帧层写帧头
       └─ write_payload_fn(batch)                 ← 类型层写 payload
            ├─ TryWrite(name_len)
            ├─ TryWrite(type_name)
            └─ SerializeToBatch(batch, msg)       ← pb_data
                 ├─ Reserve(pb_len)               ← 尝试直写指针
                 │   ├─ 不跨环尾 → SerializeToArray 直写 ring（零拷贝）
                 │   └─ 跨环尾 → 序列化到临时缓冲区 → TryWrite 分两段 memcpy
                 └─ Flush() + NotifyPeer()
```

`BatchWriter::TryWrite` 在环尾自动拆分为两段 memcpy：

```cpp
size_t tail = Capacity - phys_w;   // 环尾剩余空间
if (len <= tail)
    memcpy(base + phys_w, src, len);          // 不跨环尾
else {
    memcpy(base + phys_w, src, tail);          // 前半到环尾
    memcpy(base,          src + tail, len - tail);  // 后半回绕到环头
}
```

## 读取流程

父进程逐帧读取并验证：

```
for i in 0..29:
    while FrameReader::TryRecv(ch, &payload, &payload_len) != 0:
        ;   ← 数据不足时等写端写入
    DecodePayload(payload) → 跳过 [name_len][type_name]，得到 pb_data 指针
    ParseFromArray(pb_data) → 还原 protobuf 对象
    逐字段校验
```

`FrameReader::TryRecv` 处理跨环尾的读取：

```
TryRecv:
  ├─ Peek(seg1, seg1_len, seg2, seg2_len)    ← 获取环中可读数据的两段指针
  ├─ 解析 MsgHeader，确定帧长
  │
  ├─ 快速路径：payload 完全在 seg1 内
  │   └─ 直接返回 ring 内部指针（零拷贝）
  │
  └─ 慢速路径：payload 跨越 seg1/seg2 边界
      └─ 拷贝到内部 64KB 缓冲区，返回缓冲区指针
```

```
环内存:
         seg1（环尾部分）              seg2（环头部分）
  ┌──────────────────────┐    ┌────────────────────────┐
  │ ... MsgHeader│payload│    │payload 后半 ...         │
  └──────────────────────┘    └────────────────────────┘
         ▲ read_pos                                ▲ write_pos
```

跨环尾对 ProtoCodec 完全透明——FrameReader 在内部拼接数据后返回连续的 payload 指针。

## 读写配合

环容量 4096 字节，大帧 242 字节，同时最多容纳十几帧。

```
时间轴：

写端：  [帧0][帧1][帧2]...[帧N] ← 环满，spin
读端：         [消费帧0][消费帧1] ← 释放空间
写端：                            [帧N+1] ← 继续写
                                     ↑
                               可能跨环尾
```

写端环满时 `TryWrite` 返回 -1，`while` 循环等待。读端 `TryRecv` 成功后内部记录 `pending_commit`，下次 `TryRecv` 时自动 `CommitRead` 推进 read_pos，释放环空间。30 帧足够让 write_pos 绕过 4096 边界多次。

## 三个测试场景的分工

| 测试 | 方式 | 覆盖点 |
|------|------|--------|
| Test 1 | 单进程，手动控制 tail 偏移 | 精确覆盖 10 个拆分位置：MsgHeader 中间、name_len 边界、type_name 中间、pb_data 起始等 |
| Test 2 | 跨进程，先填充推进再发送 | 验证 ProtoCodec::Send 的 Reserve 零拷贝路径和 TryWrite 回退路径 |
| Test 3 | 跨进程，30 帧连续收发 | 自然回绕，混合帧大小，验证长序列下的稳定性 |
