# CLAUDE.md

## Project Overview

shm-cpp is a high-performance Linux IPC library using lock-free SPSC byte ring buffers over shared memory. It provides zero-copy message passing for variable-length messages (1B to ~4MB). Header-only, C++17, Linux-only.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces: `server`, `client`, `bench_shm`, `bench_socket`, `test_codec`.

## Run Tests

```bash
./build/test_codec
```

No test framework — tests use `assert()` and print `=== TEST PASSED ===` on success. The test forks a child process (sender) and uses socketpair for IPC.

## Run Benchmarks

```bash
./build/bench_shm      # shared memory echo ping-pong
./build/bench_socket    # unix socket echo ping-pong (baseline)
```

## Architecture

```
include/shm_ipc/
  ringbuf.hpp         — Lock-free SPSC byte ring buffer (template, all static methods on raw shm pointer)
  common.hpp          — RAII wrappers (UniqueFd, MmapRegion), fd passing (SCM_RIGHTS), memfd/eventfd creation
  ring_channel.hpp    — Bidirectional channel: two RingBufs + memfd creation + fd exchange handshake + eventfd notify
  event_loop.hpp      — poll + timerfd single-threaded event loop
  codec.hpp           — POD encode/decode with type tags, SendPod helper, PodReader zero-copy stream reader
  messages.hpp        — Application-layer POD message definitions (Heartbeat, ClientMsg)
  client_state.hpp    — Per-connection state struct (shared by client/server)
src/
  server.cpp          — Multi-client server using EventLoop + RingChannel
  client.cpp          — Client using EventLoop + RingChannel
```

Namespace: `shm_ipc`. Default ring capacity: 8MB (template parameter).

## Code Conventions

- **Language**: C++17, Linux-only (memfd_create, timerfd, eventfd, SCM_RIGHTS)
- **Style**: Modified Google C++ Style (see `cpp-style.md` and `.clang-format`)
  - 4-space indentation, no tabs
  - Allman brace style (braces on new lines)
  - PascalCase for classes, structs, and functions (e.g. `TryWrite`, `CreateMemfd`)
  - snake_case for variables; class members have trailing `_` (e.g. `fd_`, `write_pos`)
  - Constants: `k` + PascalCase (e.g. `kSentinel`, `kMaxClients`)
  - Globals: `g` + PascalCase (e.g. `gLoop`, `gReaders`)
  - Namespaces: lowercase (e.g. `shm_ipc`)
- **Include guards**: `#ifndef SHM_IPC_<FILE>_HPP_` / `#define` / `#endif`
- **Comments**: Chinese Doxygen-style comments (`@brief`, `@param`, `@return`)
- **Header-only library**: INTERFACE CMake target, no .cpp for the library itself
- **File extensions**: `.hpp` for headers, `.cpp` for source files

## Dependencies

None beyond Linux system headers. No third-party libraries.

## Key Design Decisions

- SPSC (not MPMC) — one producer, one consumer per direction; no CAS loops needed
- `memfd_create` (not `shm_open`) — anonymous memory, no filesystem cleanup
- `eventfd` for cross-process notification — shared via SCM_RIGHTS during handshake
- `poll` (not `epoll`) — max 16 clients, poll is simpler and sufficient
- `timerfd` (not SIGALRM) — per-client independent timers, works with poll
- Ring capacity must be power of 2 (bitmask for physical offset)
- Max single message = Capacity/2 - 8 bytes (guarantees write always succeeds after sentinel)
- 8-byte aligned message frames with sentinel-based wrap-around
