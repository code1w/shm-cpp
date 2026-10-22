/**
 * @file common.hpp
 * @brief RAII 封装与 IPC 辅助工具（C++17，纯头文件）
 */

#ifndef SHM_IPC_COMMON_HPP_
#define SHM_IPC_COMMON_HPP_

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace shm {

inline constexpr const char *kDefaultSocketPath = "/tmp/fd-pass.socket";

// memfd seal 常量：glibc 2.17 等老版本的 <fcntl.h> 未定义，而
// <linux/fcntl.h> 与其 struct flock 定义冲突，故直接使用内核 UAPI 固定值
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0004U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS   1033     ///< UAPI: fcntl 命令，为文件添加 seal
#define F_SEAL_SHRINK 0x0002   ///< 禁止 ftruncate 缩小文件
#define F_SEAL_GROW   0x0004   ///< 禁止 ftruncate 扩大文件
#endif

// ---------------------------------------------------------------------------
// UniqueFd — 移动语义 RAII 文件描述符包装
// ---------------------------------------------------------------------------

/** @brief 移动语义 RAII 文件描述符包装，禁止复制 */
class [[nodiscard]] UniqueFd
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
            size_ = std::exchange(o.size_, 0);
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
 *
 * 创建后立即以 F_SEAL_SHRINK|F_SEAL_GROW 封印尺寸：memfd 经 SCM_RIGHTS
 * 发给对端后，任何持有 fd 者（含创建者）都无法再 ftruncate 改变大小，
 * 防止恶意对端握手后缩小文件使接收方映射读越界（SIGBUS）。
 * seal 不影响已建立的可写映射，本库创建后也不再改文件大小。
 *
 * @param name  memfd 名称（调试用）
 * @param size  文件大小（字节）
 * @return 持有 memfd 的 UniqueFd
 */
[[nodiscard]]
inline UniqueFd CreateMemfd(const char *name, std::size_t size)
{
    // MFD_ALLOW_SEALING 允许随后 fcntl(F_ADD_SEALS)；内核不识别该标志
    // （EINVAL）时退化为普通 memfd，仅失去防 truncate 硬化
    int fd = static_cast<int>(::syscall(
        __NR_memfd_create, name,
        static_cast<unsigned>(MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (fd < 0 && errno == EINVAL)
        fd = static_cast<int>(
            ::syscall(__NR_memfd_create, name, static_cast<unsigned>(MFD_CLOEXEC)));
    if (fd < 0)
        throw std::runtime_error(std::string("memfd_create: ") +
                                 std::strerror(errno));
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
    }
    // 封印尺寸（尽力而为：不支持 seal 的内核静默跳过，与历史行为一致）
    (void)::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
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

    ssize_t sent = ::sendmsg(socket, &msg, 0);
    if (sent < 0)
        throw std::runtime_error(std::string("sendmsg: ") + std::strerror(errno));
    if (static_cast<std::size_t>(sent) < io.iov_len)
        throw std::runtime_error("sendmsg: partial send");
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

    ssize_t received = ::recvmsg(socket, &msg, 0);
    if (received < 0)
        throw std::runtime_error(std::string("recvmsg: ") + std::strerror(errno));
    if (received == 0)
        throw std::runtime_error("recvmsg: connection closed during fd transfer");

    // 消息至少应包含 4 字节类型标签，否则说明对端行为异常，
    // 继续解析会读到未初始化的栈数据
    if (static_cast<std::size_t>(received) < sizeof(uint32_t))
        throw std::runtime_error("recvmsg: message too short for fd tag");

    // 控制消息被截断（对端一次发送了多个 fd 或缓冲区不足）时静默继续
    // 会导致 fd 泄漏或标签错位，必须显式报错
    if (msg.msg_flags & MSG_CTRUNC)
        throw std::runtime_error("recvmsg: control message truncated");

    auto *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
        throw std::runtime_error("RecvFd: no SCM_RIGHTS in control message");
    if (cmsg->cmsg_len < CMSG_LEN(sizeof(int)))
        throw std::runtime_error("RecvFd: control message too short for fd");
    // 对端一条消息携带多个 fd 时只取第一个会造成其余 fd 泄漏，
    // 属协议违规：关闭全部并报错
    if (cmsg->cmsg_len > CMSG_LEN(sizeof(int)))
    {
        int nfds = static_cast<int>((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < nfds; ++i)
        {
            int extra = -1;
            std::memcpy(&extra, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            if (extra >= 0)
                ::close(extra);
        }
        throw std::runtime_error("RecvFd: multiple fds in one message");
    }
    int fd = -1;
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

/**
 * @brief 从 uri 中提取 Unix socket 路径
 *
 * 支持 "shm:///path"、"uds:///path"、"unix:///path" 形式，无 scheme 时原样返回。
 */
[[nodiscard]]
inline std::string UriToPath(std::string_view uri)
{
    auto pos = uri.find("://");
    if (pos == std::string_view::npos)
        return std::string(uri);
    return std::string(uri.substr(pos + 3));
}

/**
 * @brief 创建并绑定 Unix domain socket 监听端
 * @param path    socket 文件路径（已存在则先 unlink）
 * @param backlog listen 积压队列长度
 * @return 持有监听 socket 的 UniqueFd
 */
[[nodiscard]]
inline UniqueFd ListenUnixSocket(const char *path, int backlog = 16)
{
    // strncpy 超长会静默截断到错误路径，必须显式拒绝
    if (std::strlen(path) >= sizeof(sockaddr_un::sun_path))
        throw std::runtime_error(std::string("unix socket path too long: ") + path);

    UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    ::unlink(path);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::bind(sfd.Get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(sfd.Get(), backlog) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));

    return sfd;
}

/**
 * @brief 连接到指定路径的 Unix domain socket
 * @param path socket 文件路径
 * @return 已连接的 UniqueFd
 */
[[nodiscard]]
inline UniqueFd ConnectUnixSocket(const char *path)
{
    // 同 ListenUnixSocket：超长路径静默截断会连到错误地址
    if (std::strlen(path) >= sizeof(sockaddr_un::sun_path))
        throw std::runtime_error(std::string("unix socket path too long: ") + path);

    UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(sfd.Get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error(std::string("connect: ") + std::strerror(errno));

    return sfd;
}

}  // namespace shm

#endif  // SHM_IPC_COMMON_HPP_
