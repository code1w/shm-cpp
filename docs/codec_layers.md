# 消息序列化分层架构

## 总览

消息从用户侧的结构体到写入共享内存 ring buffer，经过三层处理。每层只关心自己的职责，通过明确的接口与上下层交互。

```
┌─────────────────────────────────────────────────────┐
│  应用层                                              │
│  ClientMsg / Heartbeat / protobuf Message            │
└──────────────────────┬──────────────────────────────┘
                       │ 用户调用 PodCodec<T> 或 ProtoCodec<T>
                       ▼
┌─────────────────────────────────────────────────────┐
│  类型编解码层（pod_codec.hpp / proto_codec.hpp）      │
│  职责：序列化具体类型，写入类型标识 + 数据体           │
│  产出：payload 字节流                                │
└──────────────────────┬──────────────────────────────┘
                       │ 调用 SendFrame / EncodeFrame
                       ▼
┌─────────────────────────────────────────────────────┐
│  帧层（codec.hpp / frame_reader.hpp）                │
│  职责：管理 MsgHeader（len + seq），帧的边界划分       │
│  产出：[MsgHeader 8B][payload NB] 完整帧             │
└──────────────────────┬──────────────────────────────┘
                       │ 调用 BatchWriter.TryWrite / Peek+CommitRead
                       ▼
┌─────────────────────────────────────────────────────┐
│  传输层（ringbuf.hpp / ring_channel.hpp）             │
│  职责：原始字节流的写入/读取，环回绕处理，通知对端      │
│  产出：字节流进入共享内存                              │
└─────────────────────────────────────────────────────┘
```

## 各层详解

### 传输层：ringbuf.hpp / ring_channel.hpp

**职责**：提供无锁 SPSC 字节流通道。不理解消息格式，只搬运字节。

**核心 API**：

| 写入 | 读取 |
|------|------|
| `BatchWriter::TryWrite(data, len)` — 写入字节 | `Peek(seg1, seg2)` — 零拷贝窥探可读数据 |
| `BatchWriter::Reserve(len)` — 获取直写指针 | `CommitRead(n)` — 推进读位置 |
| `BatchWriter::Flush()` — 原子发布 | `ReadExact(buf, len)` — 阻塞读取 |

**关键设计**：
- 环容量必须是 2 的幂，用位掩码计算物理偏移
- BatchWriter 累积多次写入，一次 `Flush` 做单次 `release store`
- `Reserve` 在不跨环尾时返回直写指针，支持上层零拷贝写入
- RingChannel 封装双向通道 + eventfd 通知 + memfd/fd 交换握手

**这层不知道什么**：消息边界在哪里、payload 是什么类型。

---

### 帧层：codec.hpp + frame_reader.hpp

**职责**：在字节流上划分消息边界。负责 `MsgHeader` 的写入和解析。

帧格式：

```
┌──────────────────┬────────────────────────────────┐
│ MsgHeader (8B)   │ payload (N bytes)              │
│  len: u32        │  （由上层类型编解码层定义内容）   │
│  seq: u32        │                                │
└──────────────────┴────────────────────────────────┘
```

**写入侧 API**（codec.hpp）：

```cpp
// 缓冲区编码：写 MsgHeader，返回 payload 指针供上层填充
char *EncodeFrame(buf, buf_size, payload_len, seq);

// Ring 发送：写 MsgHeader + 回调写 payload，自动 Flush
int SendFrame(ch, payload_len, seq, write_payload_fn);

// 批量发送：同上但不 Flush（多帧攒批后统一发布）
int SendFrameBatch(batch, payload_len, seq, write_payload_fn);
```

上层只需告诉帧层 "payload 多长、seq 多少"，然后在回调里写自己的 payload。MsgHeader 的构造完全由帧层负责。

**读取侧**（frame_reader.hpp）：

```cpp
// 从 ring 字节流中提取一个完整帧，返回 payload 指针
int FrameReader::TryRecv(ch, &payload, &payload_len);
```

FrameReader 通过 `Peek` 窥探 ring 中的数据，解析 MsgHeader 确定帧长，当数据不跨环尾时直接返回 ring 内部指针（零拷贝快速路径），跨环尾时拷贝到内部缓冲区。

**这层不知道什么**：payload 里面装的是 POD 还是 protobuf、有没有 tag 前缀。

---

### 类型编解码层：pod_codec.hpp / proto_codec.hpp

**职责**：将具体类型序列化为 payload 字节、从 payload 反序列化回结构体。定义 payload 的内部格式。

#### PodCodec\<T\>

适用于 `trivially_copyable` 的 POD 结构体。

payload 格式：

```
┌──────────┬──────────────────┐
│ tag (4B) │ T 原始字节 (NB)   │
│ u32      │ memcpy 直写       │
└──────────┴──────────────────┘
```

- `tag` 是编译期通过 `SHM_IPC_REGISTER_POD(Type, value)` 注册的 4 字节类型标识
- 编码 = memcpy，解码 = memcpy，零开销

核心方法：

```cpp
// 编码到缓冲区：调用 EncodeFrame 写帧头，自己写 [tag][T]
static uint32_t EncodeTo(obj, buf, buf_size, seq);

// 发送到 ring：调用 SendFrame，回调写 [tag][T]
static int Send(ch, obj, seq);

// 反序列化：校验大小后 memcpy
static bool DecodeFrom(payload, payload_len, out);
```

#### ProtoCodec\<T\>

适用于 protobuf Message 类型。

payload 格式：

```
┌──────────────────┬────────────────┬───────────────┐
│ name_len (2B)    │ type_name (NB) │ pb_data (NB)  │
│ u16              │ 如 "pkg.Msg"   │ protobuf 编码  │
└──────────────────┴────────────────┴───────────────┘
```

- `type_name` 来自 `Message::GetTypeName()`，支持接收端按名称分发
- `pb_data` 由 `SerializeToArray` 生成

核心方法：

```cpp
// 编码到缓冲区：调用 EncodeFrame 写帧头，自己写 [name_len][name][pb]
static uint32_t EncodeTo(msg, buf, buf_size, seq);

// 发送到 ring：调用 SendFrame，回调写 payload
// pb_data 优先直写 ring（Reserve 零拷贝），跨环尾回退到临时缓冲区
static int Send(ch, msg, seq);

// 从 payload 跳过 type_name 前缀，返回 pb_data 指针
bool DecodePayload(payload, payload_len, &data, &data_len);

// 反序列化：ParseFromArray
static bool DecodeFrom(payload, payload_len, out);
```

**这层不知道什么**：MsgHeader 的格式、ring buffer 的容量和回绕机制。

---

## 数据流示例

### 写入路径（以 ProtoCodec 发送 protobuf 为例）

```
ProtoCodec<T>::Send(ch, msg, seq)
  │
  ├─ 计算 payload_len = 2 + type_name.size() + pb_size
  │
  └─ SendFrame(ch, payload_len, seq, lambda)     ← 帧层
       │
       ├─ 检查 ring 可写空间 ≥ 8 + payload_len
       ├─ StartBatch()
       ├─ TryWrite(MsgHeader)                    ← 帧层写帧头
       │
       └─ lambda(batch)                          ← 回调，类型层写 payload
            ├─ TryWrite(name_len)
            ├─ TryWrite(type_name)
            └─ SerializeToBatch(batch, msg)
                 ├─ Reserve(pb_len)              ← 传输层：尝试直写指针
                 │   ├─ 成功 → SerializeToArray 直写 ring（零拷贝）
                 │   └─ 失败（跨环尾）→ 序列化到临时缓冲区 → TryWrite
                 └─ CommitReserve / TryWrite     ← 传输层
       │
       └─ Flush() + NotifyPeer()                 ← 传输层
```

### 读取路径

```
ProtoCodec<T>::Recv(ch, out)
  │
  └─ FrameReader::TryRecv(ch, &payload, &payload_len)  ← 帧层
       │
       ├─ Peek(seg1, seg2)                              ← 传输层
       ├─ 解析 MsgHeader，确定帧长
       ├─ 不跨环尾 → 零拷贝返回 ring 内部指针
       │  跨环尾 → 拷贝到内部缓冲区
       └─ 返回 payload 指针 + 长度
  │
  ├─ DecodePayload(payload) → 跳过 [name_len][name]，得到 pb_data
  └─ ParseFromArray(pb_data) → T
```

## ICodec 接口

`codec_interface.hpp` 定义了类型无关的虚接口 `ICodec`，使调用方（如 server.cpp 的 client_state）不需要知道具体是 PodCodec 还是 ProtoCodec：

```cpp
class ICodec {
    virtual uint32_t Encode(msg, buf, size, seq) = 0;     // 编码到缓冲区
    virtual bool Decode(buf, len, &payload, &len, &seq);   // 从缓冲区解码
    virtual bool DecodePayload(payload, len, &data, &len); // 跳过类型前缀
    virtual int Send(ch, msg, seq) = 0;                    // 发送到 ring
    virtual int Recv(ch, out) = 0;                         // 从 ring 接收
    virtual void Commit(ch) = 0;                           // 提交上一帧读取
    virtual string TypeName() const = 0;                   // 类型标识
};
```

## 文件与层次对应

| 层次 | 文件 | 核心概念 |
|------|------|----------|
| 传输层 | `ringbuf.hpp`, `ring_channel.hpp` | RingBuf, BatchWriter, RingChannel, Peek/CommitRead |
| 帧层 | `codec.hpp`, `frame_reader.hpp` | MsgHeader, EncodeFrame, SendFrame, FrameReader |
| 类型编解码层 | `pod_codec.hpp`, `proto_codec.hpp` | PodCodec, ProtoCodec, TypeTag, type_name |
| 接口抽象 | `codec_interface.hpp` | ICodec |
