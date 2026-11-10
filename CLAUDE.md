# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

shm-cpp is a high-performance Linux IPC library using lock-free SPSC byte ring buffers over shared memory. It provides zero-copy message passing for variable-length messages (1B to ~4MB). Header-only, C++17, Linux-only.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces: `server`, `client`, `socket_server`, `socket_client`, `app_server`, `app_client`, `bench_shm`, `bench_socket`, `bench_batch`, `bench_codec`, `test_codec`, `test_ringbuf_wrap`, `test_eventloop`, `test_shm_app` (plus `test_proto_codec`, `test_proto_wrap` when protobuf is found under `3rd/protobuf`).

## Run Tests

```bash
./build/test_codec
./build/test_ringbuf_wrap
./build/test_ringbuf_boundary   # 满环/Reserve/空指针/moved-from/SPSC 压测
./build/test_eventloop
./build/test_shm_app    # 上层接口（ShmServer/ShmAsyncClient/ShmClient + comm 接口层）集成测试
```

No test framework — tests use `assert()` and print `=== TEST PASSED ===` on success. Test targets are built with `-UNDEBUG` so asserts stay live in Release builds. `test_codec` forks a child process (sender) and uses socketpair for IPC.

## Run Benchmarks

```bash
./build/bench_shm      # shared memory echo ping-pong
./build/bench_socket    # unix socket echo ping-pong (baseline)
./build/bench_batch     # batch write throughput
```

## Architecture

```
include/shm_ipc/
  error.hpp           — 统一错误码 ErrorCode + errno 归类（公共 API 不抛显式异常）
  ringbuf.hpp         — Lock-free SPSC byte ring buffer (template, all static methods on raw shm pointer)
  common.hpp          — RAII wrappers (UniqueFd, MmapRegion), fd passing (SCM_RIGHTS), memfd/eventfd creation
  ring_channel.hpp    — Bidirectional channel: two RingBufs + memfd creation + fd exchange handshake + eventfd notify
  event_loop.hpp      — poll + timerfd single-threaded event loop
  codec.hpp           — Frame layer: [MsgHeader 8B][payload] encode/decode, Send/SendFrame helpers, SHM_IPC_REGISTER_POD
  frame_reader.hpp    — FrameReader: zero-copy stateful stream frame extractor (Peek + CommitRead)
  codec_interface.hpp — ICodec abstract interface (type-erased encode/decode/send/recv)
  pod_codec.hpp       — PodCodec<T>: POD messages, payload = [tag u32][T bytes], tag verified on receive
  proto_codec.hpp     — ProtoCodec<T>: protobuf messages, payload = [type_name_len u16][type_name][pb bytes]
  messages.hpp        — Application-layer POD message definitions (Heartbeat, ClientMsg)
  client_state.hpp    — Per-connection state struct (shared by client/server)
  simple_pod_codec.hpp — codec::SimplePodCodec：std::any 模型的通用 POD codec（[tag u32][T bytes]）
  shm_connection.hpp  — 上层接口：ShmConnection（实现 comm::ITcpConnection，std::any + SendCmd 模型）
  shm_server.hpp      — 上层接口：ShmServer（实现 comm::ITcpServer）
  shm_async_client.hpp — 上层接口：ShmAsyncClient（实现 comm::ITcpAsyncClient）
  shm_client.hpp      — 上层接口：ShmClient（实现 comm::ITcpClient，同步裸字节流）
include/comm/         — 与 framework/io 共享的接口层（应用代码只依赖它，实现可替换）
  types.h             — 回调签名/枚举（与 io/defines.h 的 tcp 部分对齐）
  codec.h             — codec::Codec 接口（与框架同签名）+ codec::Adapt 鸭类型适配器
  connection.h        — ITcpConnection（对应框架 tcp::TcpConnection 的应用层 API）
  server.h            — ITcpServer（对应框架 io::TcpServer）
  client.h            — ITcpAsyncClient（对应 io::TcpAsyncClient）+ ITcpClient（对应 io::tcp::Client）
src/
  server.cpp          — Multi-client server using EventLoop + RingChannel
  client.cpp          — Client using EventLoop + RingChannel
  app_server.cpp      — shm::ShmServer 上层接口示例（echo 服务）
  app_client.cpp      — shm::ShmAsyncClient 上层接口示例
```

Namespace: `shm` (headers live in `include/shm_ipc/`). Default ring capacity: 8MB (template parameter).

## Code Conventions

- **Language**: C++17, Linux-only (memfd_create, timerfd, eventfd, SCM_RIGHTS)
- **Style**: Modified Google C++ Style (see `cpp-style.md` and `.clang-format`)
  - 4-space indentation, no tabs
  - Allman brace style (braces on new lines)
  - PascalCase for classes, structs, and functions (e.g. `TryWrite`, `CreateMemfd`)
  - snake_case for variables; class members have trailing `_` (e.g. `fd_`, `write_pos`)
  - Constants: `k` + PascalCase (e.g. `kMaxClients`)
  - Globals: `g` + PascalCase (e.g. `gLoop`, `gReaders`)
  - Namespaces: lowercase (e.g. `shm`)
- **Include guards**: `#ifndef SHM_IPC_<FILE>_HPP_` / `#define` / `#endif`
- **Comments**: Chinese Doxygen-style comments (`@brief`, `@param`, `@return`)
- **Header-only library**: INTERFACE CMake target, no .cpp for the library itself
- **File extensions**: `.hpp` for headers, `.cpp` for source files

## Dependencies

Linux system headers plus pthread (`shm_server.hpp` 内部持有握手线程，INTERFACE 目标通过 `Threads::Threads` 传递）。Optional: protobuf (under `3rd/protobuf`) enables `proto_codec.hpp` targets.

## Key Design Decisions

- SPSC (not MPMC) — one producer, one consumer per direction; no CAS loops needed
- `memfd_create` (not `shm_open`) — anonymous memory, no filesystem cleanup
- `eventfd` for cross-process notification — shared via SCM_RIGHTS during handshake
- `poll` (not `epoll`) — max 16 clients, poll is simpler and sufficient
- `timerfd` (not SIGALRM) — per-client independent timers, works with poll
- EventLoop 增删用墓碑机制 — RemoveFd/RemoveTimer 立即生效（含同一分发批次内，被取消的定时器不会用旧 revents 补触发），条目在分发结束后回收
- ShmServer 握手在后台线程 — accept 后 fd 交给握手线程完成 fd 交换（5s 超时），恶意/僵死连接不会阻塞事件循环；完成后经 eventfd 通知 loop 线程收编
- ShmClient::Close 线程安全 — 置原子标志 + shutdown socket 唤醒阻塞读线程，资源回收延迟到 Connect()/析构
- Ring capacity must be power of 2 (bitmask for physical offset)
- Max single message = Capacity/2 (guarantees contiguous free space always available); `SendFrameBatch` 分块写入可接近 Capacity，FrameReader 的 -3 上限为 Cap - 8
- Pure byte-stream ring buffer — no framing at ringbuf layer; codec layer adds type-tagged frames on top
- SendFrame/SendFrameBatch 异常安全 — write_payload 回调抛异常时 Cancel/RewindTo 回滚未发布写入后重新抛出；放任析构 Flush 会向对端发布半截帧，永久破坏字节流（test_codec Test 15 回归）
- memfd 创建即封印（F_SEAL_SHRINK|F_SEAL_GROW）— 防恶意对端握手后 ftruncate 缩小文件使接收方读映射越界（SIGBUS）；seal 不影响已建立的可写映射
- ~ShmAsyncClient 必须 Close 连接 — 摘除可能挂起的验证定时器（回调捕获裸 this，不摘除即 UAF；test_shm_app Part 3 回归）
- ShmServer::Start 幂等 — Listen 入口 started_ 守卫，二次 Start 直接成功（否则对 joinable 握手线程 move 赋值 = std::terminate；test_shm_app Part 4 回归）
- 握手校验对端 memfd 尺寸 — 收到 memfd 后先 fstat 比对 `Ring::shm_size` 再 mmap；两端 Capacity 不一致或恶意 0 字节 fd 会直接拒绝，而不是在环尾访问时 SIGBUS（test_codec Test 16 回归）
- FrameReader 回退缓冲按需增长 — `BufSize` 仅作初始 reserve，跨环尾的大帧（发送端允许到 Capacity/2）不再因 64KB 固定缓冲被判毒帧丢弃；`-2` 仅表示显式配置的 `MaxBufSize` 上限被超过
- BatchWriter 契约收紧 — moved-from 对象拒绝写入；`CommitReserve` 在 release 下也校验"已 Reserve 且长度一致"；`CommitRead`/`RewindTo` 越界时 debug assert、release 钳制
- SendFd/RecvFd 处理 SOCK_STREAM 短读短写 — SCM_RIGHTS 随首字节送达，剩余固定载荷循环补齐，避免后续握手消息流错位
- RingBuf::Init 原地构造 RingHeader — 原子对象必须有生命周期起点（不能只对 mmap 原始内存 memset），并校验 64B 对齐
- 公共 API 不抛显式异常 — 失败返回 `shm::ErrorCode`（`error.hpp`），系统调用 errno 通过可选的 `int* sys_errno` 出参返回；对象生产接口统一用出参（`CreateRingChannel`/`StartBatch`/`CreateMemfd`/`CreateEventfd`/`ListenUnixSocket`/`ConnectUnixSocket`/`SendFd`/`RecvFd`/`AddTimer`/`AddOneShotTimer`）
- 标准库异常在入口转换 — `std::bad_alloc`→`kNoMemory`、`std::system_error`（线程创建）→`kSystem`，在 ShmServer/ShmAsyncClient/ShmClient/EventLoop 的公共入口 catch；用户回调异常仍按原语义传播（codec 回滚、EventLoop 回收墓碑后 rethrow）
- `UriToPath` 返回 `std::string_view` — 握手路径不再分配 `std::string`；unlink 走栈缓冲 `UnlinkUnixSocket`
- 数据区页对齐 + 镜像映射 — `kRingDataOffset=4096`，`RingChannel` 通过 `MmapRegion::MapMirrored` 把数据区连续映射两圈；跨环尾读写单次 memcpy、`Peek` 永远单段。镜像失败时自动退化为普通映射，两段路径保持可用
- 通知合并可选 — `ShmServer/ShmAsyncClient::SetDeferredNotify(true)` 后，同一事件循环分发批次内的多次 `SendCmd` 合并为一次 eventfd（默认关闭，保持逐条通知的低延迟语义）
- `ShmClient::PeekMessage/CommitMessage` — 同步客户端的零拷贝读取；指针在 Commit/下一次 Peek/Close 前有效
- `SimplePodCodec` 热路径 — 扁平注册表 + 普通函数指针，避免 `unordered_map`×2 + `std::function`；`Decode` 在数据不足时通过 `size` 出参报告当前帧所需长度
- 同类型消息复用 `std::any` — `Decode` 写入已有 any（同类型原地覆盖）；`ShmConnection::DrainRing` 在整个 drain 中复用同一个 any，避免每条消息构造/析构与堆分配（大对象受益；回调内引用不得外存）
- `SendCmd` 直写 ring — `comm::codec::Codec::EncodedSize` 报告精确长度时，`SendCmd` 对 ≥256B 消息直接 `Reserve + Encode` 写入 ring（单次拷贝），并支持到 `max_write_size`（默认 4MB）；未知长度的 codec 回退 `encode_buf_`
- `ChannelBatchWriter::FlushNoNotify` — 发布 write_pos 但不通知，供 `SendCmd` 保留延迟通知合并语义
