// ring_channel.hpp — Bidirectional shared-memory ring channel (C++17)
//
// Wraps a pair of byte ring buffers (one for writing, one for reading) and
// encapsulates the memfd creation, eventfd creation, and fd-exchange handshake.
//
// Notification: each direction has a shared eventfd. The reader creates it and
// sends it to the writer via SCM_RIGHTS. The writer calls notify_peer() after
// writing to the ring; the reader polls notify_read_fd() for POLLIN.

#ifndef SHM_IPC_RING_CHANNEL_HPP
#define SHM_IPC_RING_CHANNEL_HPP

#include "common.hpp"
#include "ringbuf.hpp"

#include <sys/mman.h>

namespace shm_ipc {

template <std::size_t Capacity = 8 * 1024 * 1024>
class RingChannel {
public:
    using Ring = RingBuf<Capacity>;

    RingChannel() = default;

    // Move-only (owns memfds, mmap regions, eventfds)
    RingChannel(RingChannel &&) = default;
    RingChannel &operator=(RingChannel &&) = default;
    RingChannel(const RingChannel &) = delete;
    RingChannel &operator=(const RingChannel &) = delete;

    /// Client-side factory.
    ///
    /// Handshake (4 fd exchanges over socket_fd):
    ///   1. send memfd_c2s        →  server
    ///   2. send efd_client       →  server  (server writes to this to wake client)
    ///   3. recv memfd_s2c        ←  server
    ///   4. recv efd_server       ←  server  (client writes to this to wake server)
    [[nodiscard]] static RingChannel connect(int socket_fd) {
        RingChannel ch;

        // Create write ring (client→server)
        ch.write_memfd_ = create_memfd("client_to_server", Ring::shm_size);
        ch.write_region_ = MmapRegion{ch.write_memfd_.get(), Ring::shm_size,
                                      PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::init(ch.write_region_.get());

        // Create our "wake me" eventfd
        ch.notify_read_efd_ = create_eventfd();

        // Send: memfd_c2s, efd_client
        send_fd(socket_fd, ch.write_memfd_.get(), FdTag::kMemfd);
        send_fd(socket_fd, ch.notify_read_efd_.get(), FdTag::kEventfd);

        // Receive: memfd_s2c, efd_server
        ch.read_memfd_ = recv_fd_expect(socket_fd, FdTag::kMemfd);
        ch.read_region_ = MmapRegion{ch.read_memfd_.get(), Ring::shm_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED};
        ch.notify_write_efd_ = recv_fd_expect(socket_fd, FdTag::kEventfd);

        return ch;
    }

    /// Server-side factory.
    ///
    /// Handshake (4 fd exchanges over socket_fd):
    ///   1. recv memfd_c2s        ←  client
    ///   2. recv efd_client       ←  client  (server writes to this to wake client)
    ///   3. send memfd_s2c        →  client
    ///   4. send efd_server       →  client  (client writes to this to wake server)
    [[nodiscard]] static RingChannel accept(int socket_fd) {
        RingChannel ch;

        // Receive: memfd_c2s, efd_client
        ch.read_memfd_ = recv_fd_expect(socket_fd, FdTag::kMemfd);
        ch.read_region_ = MmapRegion{ch.read_memfd_.get(), Ring::shm_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED};
        ch.notify_write_efd_ = recv_fd_expect(socket_fd, FdTag::kEventfd);

        // Create write ring (server→client)
        ch.write_memfd_ = create_memfd("server_to_client", Ring::shm_size);
        ch.write_region_ = MmapRegion{ch.write_memfd_.get(), Ring::shm_size,
                                      PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::init(ch.write_region_.get());

        // Create our "wake me" eventfd
        ch.notify_read_efd_ = create_eventfd();

        // Send: memfd_s2c, efd_server
        send_fd(socket_fd, ch.write_memfd_.get(), FdTag::kMemfd);
        send_fd(socket_fd, ch.notify_read_efd_.get(), FdTag::kEventfd);

        return ch;
    }

    // ---- Data operations ----

    int try_write(const void *data, uint32_t len, uint32_t seq) {
        return Ring::try_write(write_region_.get(), data, len, seq);
    }

    void force_write(const void *data, uint32_t len, uint32_t seq) {
        Ring::force_write(write_region_.get(), data, len, seq);
    }

    int try_read(void *data, uint32_t *len, uint32_t *seq) {
        return Ring::try_read(read_region_.get(), data, len, seq);
    }

    int peek_read(const void **data, uint32_t *len, uint32_t *seq) {
        return Ring::peek_read(read_region_.get(), data, len, seq);
    }

    void commit_read(uint32_t len) {
        Ring::commit_read(read_region_.get(), len);
    }

    uint64_t readable() const {
        return Ring::available(read_region_.get());
    }

    uint64_t writable_bytes() const {
        return Capacity - Ring::available(write_region_.get());
    }

    // ---- Notification ----

    /// fd to poll (POLLIN) for "peer wrote new data to our read ring".
    [[nodiscard]] int notify_read_fd() const noexcept { return notify_read_efd_.get(); }

    /// Signal the peer that we wrote new data to our write ring.
    void notify_peer() const {
        uint64_t v = 1;
        ::write(notify_write_efd_.get(), &v, sizeof(v));
    }

    /// Drain an eventfd (call from the notification callback).
    static uint64_t drain_notify(int efd) {
        uint64_t v = 0;
        ::read(efd, &v, sizeof(v));
        return v;
    }

    /// Maximum payload size for a single message.
    static constexpr std::size_t max_msg_size = Ring::max_msg_size;

private:
    UniqueFd   write_memfd_;
    MmapRegion write_region_;
    UniqueFd   read_memfd_;
    MmapRegion read_region_;

    UniqueFd notify_write_efd_;  // we write to this to wake the peer
    UniqueFd notify_read_efd_;   // peer writes to this to wake us
};

/// Convenience alias for the default configuration (8MB ring, ~4MB max message).
using DefaultRingChannel = RingChannel<>;

} // namespace shm_ipc

#endif // SHM_IPC_RING_CHANNEL_HPP
