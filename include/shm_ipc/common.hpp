// common.hpp — RAII wrappers and IPC helpers (C++17, header-only)

#ifndef SHM_IPC_COMMON_HPP
#define SHM_IPC_COMMON_HPP

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <linux/memfd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace shm_ipc {

inline constexpr const char *kDefaultSocketPath = "/tmp/fd-pass.socket";

// ---------------------------------------------------------------------------
// UniqueFd — move-only RAII wrapper for a file descriptor
// ---------------------------------------------------------------------------

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd &operator=(UniqueFd &&o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// MmapRegion — move-only RAII wrapper for an mmap region
// ---------------------------------------------------------------------------

class MmapRegion {
public:
    MmapRegion() = default;
    MmapRegion(int fd, std::size_t size, int prot, int flags) : size_(size) {
        addr_ = ::mmap(nullptr, size, prot, flags, fd, 0);
        if (addr_ == MAP_FAILED)
            throw std::runtime_error(std::string("mmap: ") + std::strerror(errno));
    }
    ~MmapRegion() {
        if (addr_ != MAP_FAILED) ::munmap(addr_, size_);
    }

    MmapRegion(MmapRegion &&o) noexcept
        : addr_(std::exchange(o.addr_, MAP_FAILED)), size_(o.size_) {}
    MmapRegion &operator=(MmapRegion &&o) noexcept {
        if (this != &o) {
            if (addr_ != MAP_FAILED) ::munmap(addr_, size_);
            addr_ = std::exchange(o.addr_, MAP_FAILED);
            size_ = o.size_;
        }
        return *this;
    }
    MmapRegion(const MmapRegion &) = delete;
    MmapRegion &operator=(const MmapRegion &) = delete;

    [[nodiscard]] void *get() const noexcept { return addr_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    explicit operator bool() const noexcept { return addr_ != MAP_FAILED; }

private:
    void *addr_ = MAP_FAILED;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// Fd type tags — carried in the iovec data alongside SCM_RIGHTS
// ---------------------------------------------------------------------------

/// Tag identifying the kind of file descriptor being exchanged.
enum class FdTag : uint32_t {
    kMemfd   = 0x4D454D46, // "MEMF"
    kEventfd = 0x45564644, // "EVFD"
};

struct TaggedFd {
    UniqueFd fd;
    FdTag    tag;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Create an anonymous memfd and ftruncate it to `size` bytes.
[[nodiscard]] inline UniqueFd create_memfd(const char *name, std::size_t size) {
    int fd = static_cast<int>(::syscall(__NR_memfd_create, name, 0u));
    if (fd < 0)
        throw std::runtime_error(std::string("memfd_create: ") + std::strerror(errno));
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
    }
    return UniqueFd{fd};
}

/// Send a file descriptor over a Unix socket using SCM_RIGHTS.
/// The FdTag is carried in the iovec payload so the receiver can identify
/// the fd type without relying on strict ordering.
inline void send_fd(int socket, int fd, FdTag tag) {
    std::array<char, 256> dummy{};
    auto tag_val = static_cast<uint32_t>(tag);
    std::memcpy(dummy.data(), &tag_val, sizeof(tag_val));
    iovec io{dummy.data(), dummy.size()};

    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.data();
    msg.msg_controllen = ctrl.size();

    auto *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (::sendmsg(socket, &msg, 0) < 0)
        throw std::runtime_error(std::string("sendmsg: ") + std::strerror(errno));
}

/// Receive a tagged file descriptor from a Unix socket using SCM_RIGHTS.
/// Returns both the fd and the FdTag carried in the iovec payload.
[[nodiscard]] inline TaggedFd recv_fd(int socket) {
    std::array<char, 256> data{};
    iovec io{data.data(), data.size()};

    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.data();
    msg.msg_controllen = ctrl.size();

    if (::recvmsg(socket, &msg, 0) < 0)
        throw std::runtime_error(std::string("recvmsg: ") + std::strerror(errno));

    auto *cmsg = CMSG_FIRSTHDR(&msg);
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));

    uint32_t tag_val = 0;
    std::memcpy(&tag_val, data.data(), sizeof(tag_val));

    return TaggedFd{UniqueFd{fd}, static_cast<FdTag>(tag_val)};
}

/// Receive a tagged fd and verify it matches the expected type.
[[nodiscard]] inline UniqueFd recv_fd_expect(int socket, FdTag expected) {
    auto [fd, tag] = recv_fd(socket);
    if (tag != expected) {
        const char *exp = (expected == FdTag::kMemfd) ? "memfd" : "eventfd";
        const char *got = (tag == FdTag::kMemfd) ? "memfd" : (tag == FdTag::kEventfd) ? "eventfd" : "unknown";
        throw std::runtime_error(
            std::string("recv_fd: expected ") + exp + " but got " + got);
    }
    return std::move(fd);
}

/// Create a non-blocking eventfd for cross-process notification.
[[nodiscard]] inline UniqueFd create_eventfd() {
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
    return UniqueFd{fd};
}

} // namespace shm_ipc

#endif // SHM_IPC_COMMON_HPP
