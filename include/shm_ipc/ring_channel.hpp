// ring_channel.hpp — Bidirectional shared-memory ring channel (C++17)
//
// Wraps a pair of RingBuf regions (one for writing, one for reading) and
// encapsulates the memfd creation + fd-exchange handshake.

#ifndef SHM_IPC_RING_CHANNEL_HPP
#define SHM_IPC_RING_CHANNEL_HPP

#include "common.hpp"
#include "ringbuf.hpp"

#include <sys/mman.h>

namespace shm_ipc {

template <int SlotCount = 16, int SlotDataSize = 256>
class RingChannel {
public:
    using Ring = RingBuf<SlotCount, SlotDataSize>;

    RingChannel() = default;

    // Move-only (owns memfds and mmap regions)
    RingChannel(RingChannel &&) = default;
    RingChannel &operator=(RingChannel &&) = default;
    RingChannel(const RingChannel &) = delete;
    RingChannel &operator=(const RingChannel &) = delete;

    /// Client-side: create our write ring, send fd, receive server's read ring.
    [[nodiscard]] static RingChannel connect(int socket_fd) {
        RingChannel ch;

        // Create the client→server ring buffer
        ch.write_memfd_ = create_memfd("client_to_server", Ring::shm_size);
        ch.write_region_ = MmapRegion{ch.write_memfd_.get(), Ring::shm_size,
                                      PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::init(ch.write_region_.get());

        // Exchange: send ours, receive theirs
        send_fd(socket_fd, ch.write_memfd_.get());
        ch.read_memfd_ = recv_fd(socket_fd);
        ch.read_region_ = MmapRegion{ch.read_memfd_.get(), Ring::shm_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED};
        return ch;
    }

    /// Server-side: receive client's ring fd, create our write ring, send fd back.
    [[nodiscard]] static RingChannel accept(int socket_fd) {
        RingChannel ch;

        // Receive the client→server ring buffer
        ch.read_memfd_ = recv_fd(socket_fd);
        ch.read_region_ = MmapRegion{ch.read_memfd_.get(), Ring::shm_size,
                                     PROT_READ | PROT_WRITE, MAP_SHARED};

        // Create the server→client ring buffer
        ch.write_memfd_ = create_memfd("server_to_client", Ring::shm_size);
        ch.write_region_ = MmapRegion{ch.write_memfd_.get(), Ring::shm_size,
                                      PROT_READ | PROT_WRITE, MAP_SHARED};
        Ring::init(ch.write_region_.get());

        // Send our ring fd to the client
        send_fd(socket_fd, ch.write_memfd_.get());
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

    uint64_t readable() const {
        return Ring::available(read_region_.get());
    }

    uint64_t writable_slots() const {
        return static_cast<uint64_t>(SlotCount) - Ring::available(write_region_.get());
    }

private:
    UniqueFd   write_memfd_;
    MmapRegion write_region_;
    UniqueFd   read_memfd_;
    MmapRegion read_region_;
};

/// Convenience alias for the default configuration.
using DefaultRingChannel = RingChannel<16, 256>;

} // namespace shm_ipc

#endif // SHM_IPC_RING_CHANNEL_HPP
