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
