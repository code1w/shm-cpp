# shm-cpp

基于共享内存的高性能进程间通信 (IPC) 库，使用无锁 SPSC 字节环形缓冲区实现零拷贝消息传递，支持 4MB 以内任意长度的变长消息。

## 特性

- **无锁 SPSC 字节环形缓冲区** — 单生产者-单消费者，cache-line 对齐，原子操作保证内存序
- **变长消息** — 支持 1 字节到 ~4MB 的任意长度消息，消息在环形缓冲区中紧凑排列，8 字节对齐
- **零文件系统依赖** — 通过 `memfd_create` 创建匿名共享内存，无需 `/dev/shm` 或 tmpfs
- **fd 传递** — 利用 Unix domain socket 的 `SCM_RIGHTS` 在进程间交换 memfd 文件描述符
- **双向通信** — 每个连接包含两个独立环形缓冲区，分别用于收发
- **多客户端** — 服务端支持同时处理多个客户端（默认最多 16 个），每个客户端拥有独立的缓冲区对
- **编译期可配置** — 环形缓冲区容量为模板参数（默认 8MB），支持编译期特化
- **事件驱动** — 基于 `poll` + `timerfd` 的事件循环，无信号依赖
- **Header-only** — 库部分为纯头文件，通过 CMake INTERFACE target 引入即可使用

## 系统要求

- Linux (kernel >= 3.17，需要 `memfd_create` 和 `timerfd`)
- C++17 编译器 (GCC 7+ / Clang 5+)
- CMake >= 3.14

## 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

生成两个可执行文件：`server` 和 `client`。

## 通道版本与边界保证

通道通知使用非阻塞 eventfd（`EFD_NONBLOCK | EFD_CLOEXEC`），握手标签为 `kEventfd`。已放弃 Unix datagram socketpair 通知方案；使用该方案的旧客户端和服务端需要同步升级。`NotifyReadFd()` 用于 poll，`RingChannel::DrainNotify()` 一次读取合并计数，返回值不能视为消息数。

共享 eventfd 通过 SCM_RIGHTS 传递，两端共享其 file status flags。通信双方必须保持 `O_NONBLOCK`，不得通过 `F_SETFL` 清除该属性；握手只校验当时的属性，不提供握手后对端改属性的隔离。此版本采用遵守协议的可信对端模型。在少连接、消息量受控的使用前提下，`ShmConnection::DrainRing()` 不设置读取预算，持续处理到环空、遇到不完整消息或连接关闭；`reader_wait` 按需通知继续保留。`RingChannel::WakeReader()` 仅作为显式本地唤醒接口，不再用于自动拆分读取批次。

服务端为各在途连接独立执行握手，任务数量受 `SetMaxClients()` 约束；每条连接有 5 秒总期限，Stop 会打断并回收所有握手任务。连接结果绑定接受时的服务代次，Stop/Start 后不会收编旧结果。定时器 fd 为非阻塞；周期回调需排空计数，一次性回调已由包装层排空，再次排空安全返回 0。

边界回归可运行 `./build/test_audit_core_fixes` 和 `./build/test_audit_round2_fixes`，覆盖通知失败、fd 0、广播重入关闭、慢握手隔离、批次重启与eventfd 计数饱和、合并排空及端点验证。可通过 `-DSHM_IPC_SANITIZE=address,undefined` 构建对应 sanitizer 检查。

## 连接调用约定

`ShmConnection` 是单线程、受控codec接口：所有收发与连接管理在所属 EventLoop 线程执行，调用方在整个操作期间持有稳定的连接强引用。`Encode`、`EncodedSize`、`Decode`、`SendKeepAlive` 只做编解码，不调用连接管理/收发方法，不关闭连接、替换codec或调整连接缓冲；容量不足通过返回值与所需大小交给传输层处理。

codec及收发配置只在收发操作之外修改；不嵌套调用 `SendCmd` 或 `DrainRing`，不替换正在执行的回调。消息回调可以正常回复或 `Close`，owner已有的局部强引用负责完成收尾。Debug检查本连接的常见违约行为并assert；Release依赖该约定，热路径不额外复制连接/codec/回调的shared_ptr快照，也不移动编码缓冲所有权。

## 快速开始

终端 1 — 启动服务端：

```bash
./build/server
```

终端 2 — 启动客户端（可启动多个）：

```bash
./build/client
```

客户端每 100ms 写入一条消息，服务端每 300ms 批量读取并回复。服务端故意设置较慢的读取频率以演示背压（backpressure）行为——当环形缓冲区写满时，客户端会收到"ring full"提示。

支持同时运行多个客户端实例，每个客户端与服务端之间建立独立的双向通道，互不干扰。

## 项目结构

```
shm-cpp/
├── CMakeLists.txt
├── include/shm_ipc/
│   ├── ringbuf.hpp           # 字节环形缓冲区（变长消息，模板化容量）
│   ├── common.hpp            # RAII 封装 (UniqueFd, MmapRegion) 和 fd 传递工具
│   ├── ring_channel.hpp      # 双向环形缓冲区通道
│   └── event_loop.hpp        # poll + timerfd 事件循环
└── src/
    ├── server.cpp            # 多客户端服务端
    └── client.cpp            # 客户端
```

## 架构

### 通信流程

```
  客户端 (client)                          服务端 (server)
  ──────────────                          ──────────────
  1. 创建 memfd (client→server ring)
  2. connect() ──────────────────────────→ accept()
  3. send_fd(memfd) ────────────────────→ recv_fd() → mmap
  4. recv_fd() ←──────────────────────── send_fd(memfd)  ← 创建 memfd (server→client ring)
     ↓ mmap
  ═══════════════ 双向通道建立完成 ═══════════════════

  5. try_write() → 环形缓冲区 ──(shm)──→ try_read()     # client→server 数据流
     write(eventfd) 通知 ──────────────────→                 # 唤醒读端
  6. try_read()  ←──(shm)── 环形缓冲区 ← try_write()    # server→client 数据流
                 ←──────────────────── write(eventfd) 通知   # 唤醒读端
  7. shutdown(socket) ──────────────────────→ 断开 + 清理     # 关闭连接
```

### 共享内存布局

每个环形缓冲区在共享内存中的布局（默认 Capacity = 8MB）：

```
偏移量    内容
──────────────────────────────────────────────
0x000     RingHeader (128 字节，cache-line 对齐)
            ├─ write_pos (atomic u64)       写入字节偏移（单调递增）
            ├─ _pad1[56]                    cache-line 隔离
            ├─ read_pos  (atomic u64)       读取字节偏移（单调递增）
            └─ _pad2[56]

0x080     数据区 (Capacity 字节的连续字节环形缓冲区)
            消息在数据区中紧凑排列，按 8 字节对齐：

            ┌──────────┬──────────┬─────────────────────┬───────┐
            │ len (u32)│ seq (u32)│ payload (len 字节)  │ pad   │
            │ 数据长度  │ 序列号   │ 变长载荷 (0~4MB)     │ 对齐  │
            └──────────┴──────────┴─────────────────────┴───────┘

            当数据区尾部空间不足以容纳完整消息帧时，写入哨兵
            (len = UINT32_MAX) 标记"跳转到数据区起始位置"。
```

默认配置下 (`Capacity = 8MB`)：
- 每个环形缓冲区共享内存大小：`sizeof(RingHeader) + Capacity` = 8,388,736 字节
- 单条消息最大载荷：`Capacity / 2 - 8` = 4,194,296 字节 (~4MB)
- 每个双向通道使用 2 个环形缓冲区 = ~16MB 共享内存

### 核心组件

| 组件 | 头文件 | 说明 |
|------|--------|------|
| `RingBuf<Capacity>` | `ringbuf.hpp` | 无锁字节环形缓冲区，Capacity 为数据区大小（2 的幂，默认 8MB） |
| `RingChannel<Capacity>` | `ring_channel.hpp` | 封装一对 RingBuf + memfd 创建 + fd 交换握手 |
| `EventLoop` | `event_loop.hpp` | 基于 poll + timerfd 的单线程事件循环 |
| `UniqueFd` | `common.hpp` | RAII 文件描述符 |
| `MmapRegion` | `common.hpp` | RAII mmap 映射区 |
| `send_fd` / `recv_fd` | `common.hpp` | Unix socket `SCM_RIGHTS` fd 传递 |

## API 用法

### 自定义环形缓冲区容量

```cpp
#include <shm_ipc/ring_channel.hpp>

// 16MB 容量（单条消息最大 ~8MB）
using LargeChannel = shm::RingChannel<16 * 1024 * 1024>;

auto ch = LargeChannel::connect(socket_fd);

// 发送变长消息（1 字节到 max_msg_size）
ch.try_write(data, len, seq);            // 写不下返回 -1
ch.force_write(data, len, seq);          // 写不下时丢弃最旧消息

// 接收消息
char buf[8 * 1024 * 1024];
uint32_t len, seq;
ch.try_read(buf, &len, &seq);            // 无消息返回 -1

// 查询状态
ch.readable();        // 读端已使用字节数
ch.writable_bytes();  // 写端剩余字节数
ch.max_msg_size;      // 编译期常量：单条消息最大载荷
```

### 使用事件循环

```cpp
#include <shm_ipc/event_loop.hpp>

shm::EventLoop loop;

// 添加定时器（200ms 间隔）
loop.add_timer(200, [](int tfd, short) {
    shm::EventLoop::drain_timerfd(tfd);
    // 定时逻辑 ...
});

// 监听 fd
loop.add_fd(some_fd, [](int fd, short revents) {
    // 处理事件 ...
});

loop.run();
```

### 直接使用环形缓冲区

```cpp
#include <shm_ipc/ringbuf.hpp>

// 默认 8MB 容量
using Ring = shm::RingBuf<>;

void *shm = /* mmap 得到的共享内存指针，大小 >= Ring::kShmSize */;
Ring::init(shm);

// 写入变长消息
Ring::try_write(shm, "hello", 6, /*seq=*/1);

// 写入大消息（最大 Ring::max_msg_size ≈ 4MB）
std::vector<char> big_data(4 * 1024 * 1024, 'X');
Ring::try_write(shm, big_data.data(), big_data.size(), /*seq=*/2);

// 读取
auto buf = std::make_unique<char[]>(Ring::max_msg_size);
uint32_t len, seq;
Ring::try_read(shm, buf.get(), &len, &seq);
```

## 设计决策

| 决策 | 理由 |
|------|------|
| 字节环形缓冲区而非固定槽位 | 支持 1B~4MB 变长消息，小消息不浪费空间，大消息不需要分片 |
| 8 字节对齐 + 哨兵换行 | 保证 `MsgHeader` 始终可原子读写，哨兵标记让读端跳过尾部碎片，避免消息跨缓冲区边界 |
| 单消息上限 = Capacity/2 | 保证写入时无论物理偏移在何处，要么能直接写入，要么写哨兵后从头写入一定有足够空间 |
| SPSC 而非 MPMC | 每个连接方向只有一个生产者和一个消费者，SPSC 不需要 CAS 循环，延迟更低 |
| `memfd_create` 而非 `shm_open` | 匿名内存，无需管理 `/dev/shm` 下的文件生命周期 |
| `timerfd` 而非 `SIGALRM` | timerfd 是普通 fd，可以和 socket fd 一起放入 poll；SIGALRM 是进程全局的，无法为每个客户端独立计时 |
| eventfd 按需通知 | 仅在读方准备睡眠时唤醒，计数自动合并；避免频繁 socket 消息排队 |
| Header-only 库 | 代码量小，全部为模板/inline，避免链接复杂度 |
| `poll` 而非 `epoll` | 连接数不超过 16，poll 更简单且性能差异可忽略 |

## 许可证

MIT
