# shm-cpp

基于共享内存的高性能进程间通信 (IPC) 库，使用无锁 SPSC 环形缓冲区实现零拷贝消息传递。

## 特性

- **无锁 SPSC 环形缓冲区** — 单生产者-单消费者，cache-line 对齐，原子操作保证内存序
- **零文件系统依赖** — 通过 `memfd_create` 创建匿名共享内存，无需 `/dev/shm` 或 tmpfs
- **fd 传递** — 利用 Unix domain socket 的 `SCM_RIGHTS` 在进程间交换 memfd 文件描述符
- **双向通信** — 每个连接包含两个独立环形缓冲区，分别用于收发
- **多客户端** — 服务端支持同时处理多个客户端（默认最多 16 个），每个客户端拥有独立的缓冲区对
- **编译期可配置** — 槽位数量、数据大小等参数均为模板参数，支持编译期特化
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
│   ├── ringbuf.hpp           # 模板化无锁 SPSC 环形缓冲区
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
     socket 'C' 通知 ──────────────────→                 # 唤醒读端
  6. try_read()  ←──(shm)── 环形缓冲区 ← try_write()    # server→client 数据流
                 ←──────────────────── socket 'S' 通知   # 唤醒读端
  7. socket '\0' ──────────────────────→ 断开 + 清理     # 关闭连接
```

### 共享内存布局

每个环形缓冲区在共享内存中的布局：

```
偏移量    内容
──────────────────────────────────────
0x000     RingHeader
            ├─ write_idx  (atomic u64)
            ├─ _pad1[56]               ← cache-line 隔离
            ├─ read_idx   (atomic u64)
            └─ _pad2[56]
0x080     RingSlot[0]
            ├─ seq  (u32)             消息序列号
            ├─ len  (u32)             有效数据长度
            └─ data[SlotDataSize]     载荷
0x188     RingSlot[1]
...
          RingSlot[SlotCount - 1]
```

默认配置下 (`SlotCount=16, SlotDataSize=256`)，每个环形缓冲区占用 `sizeof(RingHeader) + 16 * sizeof(RingSlot)` = 4352 字节。

### 核心组件

| 组件 | 头文件 | 说明 |
|------|--------|------|
| `RingBuf<N, S>` | `ringbuf.hpp` | 无锁环形缓冲区，N = 槽位数（2 的幂），S = 每槽数据字节数 |
| `RingChannel<N, S>` | `ring_channel.hpp` | 封装一对 RingBuf + memfd 创建 + fd 交换握手 |
| `EventLoop` | `event_loop.hpp` | 基于 poll + timerfd 的单线程事件循环 |
| `UniqueFd` | `common.hpp` | RAII 文件描述符 |
| `MmapRegion` | `common.hpp` | RAII mmap 映射区 |
| `send_fd` / `recv_fd` | `common.hpp` | Unix socket `SCM_RIGHTS` fd 传递 |

## API 用法

### 自定义环形缓冲区参数

```cpp
#include <shm_ipc/ring_channel.hpp>

// 64 个槽位，每槽 1024 字节
using MyChannel = shm_ipc::RingChannel<64, 1024>;

auto ch = MyChannel::connect(socket_fd);
ch.try_write(data, len, seq);
ch.try_read(buf, &len, &seq);
```

### 使用事件循环

```cpp
#include <shm_ipc/event_loop.hpp>

shm_ipc::EventLoop loop;

// 添加定时器（200ms 间隔）
loop.add_timer(200, [](int tfd, short) {
    shm_ipc::EventLoop::drain_timerfd(tfd);
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

using Ring = shm_ipc::RingBuf<16, 256>;

void *shm = /* mmap 得到的共享内存指针 */;
Ring::init(shm);

Ring::try_write(shm, "hello", 6, /*seq=*/1);

char buf[256];
uint32_t len, seq;
Ring::try_read(shm, buf, &len, &seq);
```

## 设计决策

| 决策 | 理由 |
|------|------|
| SPSC 而非 MPMC | 每个连接方向只有一个生产者和一个消费者，SPSC 不需要 CAS 循环，延迟更低 |
| `memfd_create` 而非 `shm_open` | 匿名内存，无需管理 `/dev/shm` 下的文件生命周期 |
| `timerfd` 而非 `SIGALRM` | timerfd 是普通 fd，可以和 socket fd 一起放入 poll；SIGALRM 是进程全局的，无法为每个客户端独立计时 |
| socket 通知字节 (`'C'`/`'S'`) | 环形缓冲区是纯共享内存轮询模型，需要一个唤醒机制避免空转；单字节通知开销极小 |
| Header-only 库 | 代码量小，全部为模板/inline，避免链接复杂度 |
| `poll` 而非 `epoll` | 连接数不超过 16，poll 更简单且性能差异可忽略 |

## 许可证

MIT
