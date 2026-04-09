/**
 * @file common.hpp
 * @brief RAII 封装与 IPC 辅助工具（C++17，纯头文件）
 */

#ifndef SHM_IPC_COMMON_HPP_
#define SHM_IPC_COMMON_HPP_

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
// UniqueFd — 移动语义 RAII 文件描述符包装
// ---------------------------------------------------------------------------

/** @brief 移动语义 RAII 文件描述符包装，禁止复制 */
class UniqueFd
{
 public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { Reset(); }

    UniqueFd(UniqueFd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd &operator=(UniqueFd &&o) noexcept
    {
        if (this != &o)
        {
            Reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    UniqueFd(const UniqueFd &)            = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    [[nodiscard]]
    int Get() const noexcept
    {
        return fd_;
    }
    int Release() noexcept { return std::exchange(fd_, -1); }
    void Reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
    explicit operator bool() const noexcept { return fd_ >= 0; }

 private:
    int fd_ = -1;  ///< 持有的文件描述符，-1 表示无效
};

// ---------------------------------------------------------------------------
// MmapRegion — 移动语义 RAII mmap 区域包装
// ---------------------------------------------------------------------------

/** @brief 移动语义 RAII mmap 区域包装，禁止复制 */
class MmapRegion
{
 public:
    MmapRegion() = default;
    MmapRegion(int fd, std::size_t size, int prot, int flags) : size_(size)
    {
        addr_ = ::mmap(nullptr, size, prot, flags, fd, 0);
        if (addr_ == MAP_FAILED)
            throw std::runtime_error(std::string("mmap: ") + std::strerror(errno));
    }
    ~MmapRegion()
    {
        if (addr_ != MAP_FAILED)
            ::munmap(addr_, size_);
    }

    MmapRegion(MmapRegion &&o) noexcept
        : addr_(std::exchange(o.addr_, MAP_FAILED)), size_(o.size_) {}
    MmapRegion &operator=(MmapRegion &&o) noexcept
    {
        if (this != &o)
        {
            if (addr_ != MAP_FAILED)
                ::munmap(addr_, size_);
            addr_ = std::exchange(o.addr_, MAP_FAILED);
            size_ = o.size_;
        }
        return *this;
    }
    MmapRegion(const MmapRegion &)            = delete;
    MmapRegion &operator=(const MmapRegion &) = delete;

    [[nodiscard]]
    void *Get() const noexcept
    {
        return addr_;
    }
    [[nodiscard]]
    std::size_t Size() const noexcept
    {
        return size_;
    }
    explicit operator bool() const noexcept { return addr_ != MAP_FAILED; }

 private:
    void *addr_       = MAP_FAILED;  ///< mmap 起始地址
    std::size_t size_ = 0;           ///< 映射长度（字节）
};

// ---------------------------------------------------------------------------
// FdTag — 随 SCM_RIGHTS 传递的文件描述符类型标签
// ---------------------------------------------------------------------------

/// @brief 文件描述符类型标签，随 iovec 数据一并传输
enum class FdTag : uint32_t
{
    kMemfd   = 0x4D454D46,  ///< "MEMF"
    kEventfd = 0x45564644,  ///< "EVFD"
};

/** @brief 携带类型标签的文件描述符 */
struct TaggedFd
{
    UniqueFd fd;  ///< 文件描述符
    FdTag tag;    ///< 类型标签
};

// ---------------------------------------------------------------------------
// 辅助函数
// ---------------------------------------------------------------------------

/**
 * @brief 创建匿名 memfd 并 ftruncate 到指定大小
 * @param name  memfd 名称（调试用）
 * @param size  文件大小（字节）
 * @return 持有 memfd 的 UniqueFd
 */
[[nodiscard]]
inline UniqueFd CreateMemfd(const char *name, std::size_t size)
{
    int fd = static_cast<int>(::syscall(__NR_memfd_create, name, 0u));
    if (fd < 0)
        throw std::runtime_error(std::string("memfd_create: ") +
                                 std::strerror(errno));
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
    }
    return UniqueFd{fd};
}

/**
 * @brief 通过 Unix socket 发送文件描述符（SCM_RIGHTS）
 * @param socket  Unix socket fd
 * @param fd      待发送的文件描述符
 * @param tag     描述 fd 类型的标签，随 iovec 一并传送
 */
inline void SendFd(int socket, int fd, FdTag tag)
{
    std::array<char, 256> dummy{};
    auto tag_val = static_cast<uint32_t>(tag);
    std::memcpy(dummy.data(), &tag_val, sizeof(tag_val));
    iovec io{dummy.data(), dummy.size()};

    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};
    msghdr msg{};
    msg.msg_iov        = &io;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl.data();
    msg.msg_controllen = ctrl.size();

    auto *cmsg       = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (::sendmsg(socket, &msg, 0) < 0)
        throw std::runtime_error(std::string("sendmsg: ") + std::strerror(errno));
}

/**
 * @brief 通过 Unix socket 接收带类型标签的文件描述符（SCM_RIGHTS）
 * @param socket  Unix socket fd
 * @return 包含 fd 与 FdTag 的 TaggedFd
 */
[[nodiscard]]
inline TaggedFd RecvFd(int socket)
{
    std::array<char, 256> data{};
    iovec io{data.data(), data.size()};

    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};
    msghdr msg{};
    msg.msg_iov        = &io;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl.data();
    msg.msg_controllen = ctrl.size();

    if (::recvmsg(socket, &msg, 0) < 0)
        throw std::runtime_error(std::string("recvmsg: ") + std::strerror(errno));

    auto *cmsg = CMSG_FIRSTHDR(&msg);
    int fd     = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));

    uint32_t tag_val = 0;
    std::memcpy(&tag_val, data.data(), sizeof(tag_val));

    return TaggedFd{UniqueFd{fd}, static_cast<FdTag>(tag_val)};
}

/**
 * @brief 接收文件描述符并校验类型标签
 * @param socket    Unix socket fd
 * @param expected  期望的 FdTag 类型
 * @return 校验通过后的 UniqueFd
 * @see RecvFd
 */
[[nodiscard]]
inline UniqueFd RecvFdExpect(int socket, FdTag expected)
{
    auto [fd, tag] = RecvFd(socket);
    if (tag != expected)
    {
        const char *exp = (expected == FdTag::kMemfd) ? "memfd" : "eventfd";
        const char *got = (tag == FdTag::kMemfd)     ? "memfd"
                          : (tag == FdTag::kEventfd) ? "eventfd"
                                                     : "unknown";
        throw std::runtime_error(std::string("recv_fd: expected ") + exp +
                                 " but got " + got);
    }
    return std::move(fd);
}

/**
 * @brief 创建用于跨进程通知的非阻塞 eventfd
 * @return 持有 eventfd 的 UniqueFd
 */
[[nodiscard]]
inline UniqueFd CreateEventfd()
{
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
    return UniqueFd{fd};
}

}  // namespace shm_ipc

#endif  // SHM_IPC_COMMON_HPP_
