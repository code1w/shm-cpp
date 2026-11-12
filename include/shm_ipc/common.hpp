/**
 * @file common.hpp
 * @brief RAII 封装与 IPC 辅助工具（C++17，纯头文件）
 *
 * 本文件所有接口均不抛显式异常：失败时返回 shm::ErrorCode，系统调用
 * 的 errno 通过可选的 int* sys_errno 出参返回。
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
#include <string_view>
#include <utility>

#include "error.hpp"

namespace shm {

inline constexpr const char* kDefaultSocketPath = "/tmp/fd-pass.socket";

// memfd seal 常量：glibc 2.17 等老版本的 <fcntl.h> 未定义，而
// <linux/fcntl.h> 与其 struct flock 定义冲突，故直接使用内核 UAPI 固定值
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0004U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033      ///< UAPI: fcntl 命令，为文件添加 seal
#define F_SEAL_SHRINK 0x0002  ///< 禁止 ftruncate 缩小文件
#define F_SEAL_GROW 0x0004    ///< 禁止 ftruncate 扩大文件
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

    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept
    {
        if (this != &o)
        {
            Reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    UniqueFd(const UniqueFd&)            = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

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

/** @brief 移动语义 RAII mmap 区域包装，禁止复制；通过 Map() 建立映射 */
class MmapRegion
{
 public:
    MmapRegion() = default;
    ~MmapRegion()
    {
        if (addr_ != MAP_FAILED)
            ::munmap(addr_, size_);
    }

    MmapRegion(MmapRegion&& o) noexcept
        : addr_(std::exchange(o.addr_, MAP_FAILED)),
          size_(o.size_),
          mirrored_(o.mirrored_),
          data_offset_(o.data_offset_)
    {
        o.mirrored_    = false;
        o.data_offset_ = 0;
    }
    MmapRegion& operator=(MmapRegion&& o) noexcept
    {
        if (this != &o)
        {
            if (addr_ != MAP_FAILED)
                ::munmap(addr_, size_);
            addr_          = std::exchange(o.addr_, MAP_FAILED);
            size_          = std::exchange(o.size_, 0);
            mirrored_      = o.mirrored_;
            data_offset_   = o.data_offset_;
            o.mirrored_    = false;
            o.data_offset_ = 0;
        }
        return *this;
    }
    MmapRegion(const MmapRegion&)            = delete;
    MmapRegion& operator=(const MmapRegion&) = delete;

    /**
     * @brief 建立映射（替换已有映射；失败时保留原映射不变）
     * @return kOk 成功；kInvalidArgument 参数非法；否则映射失败（errno 出参）
     */
    [[nodiscard]]
    ErrorCode Map(int fd, std::size_t size, int prot, int flags, int* sys_errno = nullptr) noexcept
    {
        if (fd < 0 || size == 0)
            return ErrorCode::kInvalidArgument;

        void* addr = ::mmap(nullptr, size, prot, flags, fd, 0);
        if (addr == MAP_FAILED)
        {
            if (sys_errno)
                *sys_errno = errno;
            return ErrnoToErrorCode(errno);
        }
        if (addr_ != MAP_FAILED)
            ::munmap(addr_, size_);
        addr_        = addr;
        size_        = size;
        mirrored_    = false;
        data_offset_ = 0;
        return ErrorCode::kOk;
    }

    /**
     * @brief 建立"数据区镜像"映射：同一 memfd 的 data 区连续映射两圈
     *
     * 文件布局要求：[0, data_offset) 为 header/填充页，data_offset 页对齐；
     * [data_offset, data_offset+capacity) 为数据区。映射后：
     *   base            → 文件 [0, file_size)
     *   base + file_size → 文件 [data_offset, data_offset+capacity)
     * 因此 data 区在 VA 上连续两圈，任何不超过 capacity 的窗口都连续，
     * 跨环尾的读写可用单次 memcpy。
     *
     * @param fd           memfd
     * @param file_size    文件大小（= data_offset + capacity）
     * @param data_offset  数据区在文件中的页对齐偏移
     * @param capacity     数据区大小（= 环容量，建议页对齐）
     * @param sys_errno    可选：失败时的 errno
     */
    [[nodiscard]]
    ErrorCode MapMirrored(int fd, std::size_t file_size, std::size_t data_offset, std::size_t capacity, int prot,
                          int flags, int* sys_errno = nullptr) noexcept
    {
        if (fd < 0 || capacity == 0 || data_offset + capacity != file_size)
            return ErrorCode::kInvalidArgument;

        const long page             = ::sysconf(_SC_PAGESIZE);
        const std::size_t page_size = (page > 0) ? static_cast<std::size_t>(page) : 4096u;
        if (data_offset % page_size != 0)
            return ErrorCode::kInvalidArgument;

        // VA 预留：header 页 + 两圈 data
        const std::size_t reserve = file_size + capacity;
        void* base                = ::mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED)
        {
            if (sys_errno)
                *sys_errno = errno;
            return ErrnoToErrorCode(errno);
        }

        // 第一圈：完整文件（header + data）
        if (::mmap(base, file_size, prot, flags | MAP_FIXED, fd, 0) == MAP_FAILED)
        {
            const int err = errno;
            ::munmap(base, reserve);
            if (sys_errno)
                *sys_errno = err;
            return ErrnoToErrorCode(err);
        }
        // 第二圈：仅 data 区，紧接在第一圈 data 末尾
        void* second = static_cast<char*>(base) + file_size;
        if (::mmap(second, capacity, prot, flags | MAP_FIXED, fd, static_cast<off_t>(data_offset)) == MAP_FAILED)
        {
            const int err = errno;
            ::munmap(base, reserve);
            if (sys_errno)
                *sys_errno = err;
            return ErrnoToErrorCode(err);
        }

        if (addr_ != MAP_FAILED)
            ::munmap(addr_, size_);
        addr_        = base;
        size_        = reserve;
        mirrored_    = true;
        data_offset_ = data_offset;
        return ErrorCode::kOk;
    }

    [[nodiscard]]
    void* Get() const noexcept
    {
        return addr_;
    }
    [[nodiscard]]
    std::size_t Size() const noexcept
    {
        return size_;
    }
    /// @brief 是否建立了数据区镜像映射
    [[nodiscard]]
    bool Mirrored() const noexcept
    {
        return mirrored_;
    }
    /// @brief 数据区起始指针（仅镜像映射有效）
    [[nodiscard]]
    char* DataBase() const noexcept
    {
        return static_cast<char*>(addr_) + data_offset_;
    }
    explicit operator bool() const noexcept { return addr_ != MAP_FAILED; }

 private:
    void* addr_              = MAP_FAILED;  ///< mmap 起始地址（镜像时为 VA 预留起点）
    std::size_t size_        = 0;           ///< munmap 长度（镜像时为 VA 预留长度）
    bool mirrored_           = false;       ///< 数据区是否两圈镜像
    std::size_t data_offset_ = 0;           ///< 数据区在文件中的偏移
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
 * @param name       memfd 名称（调试用，需 NUL 结尾）
 * @param size       文件大小（字节）
 * @param out        成功时写入持有 memfd 的 UniqueFd
 * @param sys_errno  可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode CreateMemfd(const char* name, std::size_t size, UniqueFd* out, int* sys_errno = nullptr) noexcept
{
    if (!out || !name || size == 0)
        return ErrorCode::kInvalidArgument;

    // MFD_ALLOW_SEALING 允许随后 fcntl(F_ADD_SEALS)；内核不识别该标志
    // （EINVAL）时退化为普通 memfd，仅失去防 truncate 硬化
    int fd =
        static_cast<int>(::syscall(__NR_memfd_create, name, static_cast<unsigned>(MFD_CLOEXEC | MFD_ALLOW_SEALING)));
    if (fd < 0 && errno == EINVAL)
        fd = static_cast<int>(::syscall(__NR_memfd_create, name, static_cast<unsigned>(MFD_CLOEXEC)));
    if (fd < 0)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0)
    {
        const int err = errno;
        ::close(fd);
        if (sys_errno)
            *sys_errno = err;
        return ErrnoToErrorCode(err);
    }
    // 封印尺寸（尽力而为：不支持 seal 的内核静默跳过，与历史行为一致）
    (void)::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
    *out = UniqueFd{fd};
    return ErrorCode::kOk;
}

/**
 * @brief 通过 Unix socket 发送文件描述符（SCM_RIGHTS）
 * @param socket    Unix socket fd
 * @param fd        待发送的文件描述符
 * @param tag       描述 fd 类型的标签，随 iovec 一并传送
 * @param sys_errno 可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode SendFd(int socket, int fd, FdTag tag, int* sys_errno = nullptr) noexcept
{
    if (socket < 0 || fd < 0)
        return ErrorCode::kInvalidArgument;

    std::array<char, 256> dummy{};
    auto tag_val = static_cast<uint32_t>(tag);
    std::memcpy(dummy.data(), &tag_val, sizeof(tag_val));

    // SOCK_STREAM 允许短写：SCM_RIGHTS 随首字节一起送达，剩余 dummy
    // 字节用不带控制消息的 send 补齐，保证接收端流对齐。
    std::size_t sent = 0;
    while (sent < dummy.size())
    {
        iovec io{dummy.data() + sent, dummy.size() - sent};

        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};
        msghdr msg{};
        msg.msg_iov    = &io;
        msg.msg_iovlen = 1;

        if (sent == 0)  // fd 只在第一条消息上携带
        {
            msg.msg_control    = ctrl.data();
            msg.msg_controllen = ctrl.size();
            auto* cmsg         = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level   = SOL_SOCKET;
            cmsg->cmsg_type    = SCM_RIGHTS;
            cmsg->cmsg_len     = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
        }

        ssize_t n;
        do
        {
            n = ::sendmsg(socket, &msg, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);

        if (n < 0)
        {
            if (sys_errno)
                *sys_errno = errno;
            return ErrnoToErrorCode(errno);
        }
        if (n == 0)
            return ErrorCode::kPeerClosed;
        sent += static_cast<std::size_t>(n);
    }
    return ErrorCode::kOk;
}

/**
 * @brief 通过 Unix socket 接收带类型标签的文件描述符（SCM_RIGHTS）
 *
 * 控制消息解析成功后用 UniqueFd 立即接管 fd：任何后续短读/错误返回都会
 * 自动关闭已收到的 fd，不会泄漏。
 *
 * @param socket    Unix socket fd
 * @param out       成功时写入 TaggedFd
 * @param sys_errno 可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode RecvFd(int socket, TaggedFd* out, int* sys_errno = nullptr) noexcept
{
    if (socket < 0 || !out)
        return ErrorCode::kInvalidArgument;

    std::array<char, 256> data{};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctrl{};

    UniqueFd owned;
    bool have_cmsg  = false;
    std::size_t got = 0;

    // SOCK_STREAM 可能短读。必须收满整条固定长度载荷，否则残留的
    // dummy 字节会被下一次 RecvFd 当成数据部分读取，导致后续消息的
    // SCM_RIGHTS 丢失/标签错位。SCM_RIGHTS 随首字节到达，只解析一次。
    while (got < data.size())
    {
        iovec io{data.data() + got, data.size() - got};

        msghdr msg{};
        msg.msg_iov    = &io;
        msg.msg_iovlen = 1;
        if (!have_cmsg)
        {
            msg.msg_control    = ctrl.data();
            msg.msg_controllen = ctrl.size();
        }

        ssize_t n;
        do
        {
            n = ::recvmsg(socket, &msg, MSG_WAITALL);
        } while (n < 0 && errno == EINTR);

        if (n < 0)
        {
            if (sys_errno)
                *sys_errno = errno;
            return ErrnoToErrorCode(errno);
        }
        if (n == 0)
            return ErrorCode::kPeerClosed;

        if (!have_cmsg)
        {
            // 控制消息被截断（对端一次发送了多个 fd 或缓冲区不足）时
            // 静默继续会导致 fd 泄漏或标签错位，必须显式报错
            if (msg.msg_flags & MSG_CTRUNC)
                return ErrorCode::kProtocol;

            auto* cmsg = CMSG_FIRSTHDR(&msg);
            if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                return ErrorCode::kProtocol;
            if (cmsg->cmsg_len < CMSG_LEN(sizeof(int)))
                return ErrorCode::kProtocol;

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
                return ErrorCode::kMultipleFds;
            }

            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
            owned.Reset(fd);
            have_cmsg = true;
        }
        got += static_cast<std::size_t>(n);
    }

    uint32_t tag_val = 0;
    std::memcpy(&tag_val, data.data(), sizeof(tag_val));
    *out = TaggedFd{std::move(owned), static_cast<FdTag>(tag_val)};
    return ErrorCode::kOk;
}

/**
 * @brief 接收文件描述符并校验类型标签
 * @param socket    Unix socket fd
 * @param expected  期望的 FdTag 类型
 * @param out       成功时写入 UniqueFd
 * @param sys_errno 可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode RecvFdExpect(int socket, FdTag expected, UniqueFd* out, int* sys_errno = nullptr) noexcept
{
    if (!out)
        return ErrorCode::kInvalidArgument;

    TaggedFd tagged{};
    ErrorCode ec = RecvFd(socket, &tagged, sys_errno);
    if (ec != ErrorCode::kOk)
        return ec;
    if (tagged.tag != expected)
        return ErrorCode::kProtocol;  // tagged.fd 由 RAII 关闭

    *out = std::move(tagged.fd);
    return ErrorCode::kOk;
}

/**
 * @brief 创建用于跨进程通知的非阻塞 eventfd
 * @param out       成功时写入 UniqueFd
 * @param sys_errno 可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode CreateEventfd(UniqueFd* out, int* sys_errno = nullptr) noexcept
{
    if (!out)
        return ErrorCode::kInvalidArgument;

    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }
    *out = UniqueFd{fd};
    return ErrorCode::kOk;
}

/**
 * @brief 从 uri 中提取 Unix socket 路径
 *
 * 支持 "shm:///path"、"uds:///path"、"unix:///path" 形式，无 scheme 时原样返回。
 * 返回 view，不分配内存；调用方需保证 uri 在 view 使用期间有效。
 */
[[nodiscard]]
inline std::string_view UriToPath(std::string_view uri) noexcept
{
    auto pos = uri.find("://");
    if (pos == std::string_view::npos)
        return uri;
    return uri.substr(pos + 3);
}

/**
 * @brief 路径写入栈上的 NUL 结尾缓冲区
 * @return false 表示路径过长或为空
 */
inline bool CopyUnixPath(std::string_view path, char* buf, std::size_t buf_size) noexcept
{
    if (path.empty() || path.size() >= buf_size)
        return false;
    std::memcpy(buf, path.data(), path.size());
    buf[path.size()] = '\0';
    return true;
}

/**
 * @brief 创建并绑定 Unix domain socket 监听端
 * @param path       socket 文件路径（已存在则先 unlink）
 * @param out        成功时写入监听 socket
 * @param backlog    listen 积压队列长度
 * @param sys_errno  可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode ListenUnixSocket(std::string_view path, UniqueFd* out, int backlog = 16,
                                  int* sys_errno = nullptr) noexcept
{
    if (!out)
        return ErrorCode::kInvalidArgument;

    char cpath[sizeof(sockaddr_un::sun_path)];
    if (!CopyUnixPath(path, cpath, sizeof(cpath)))
    {
        if (sys_errno)
            *sys_errno = ENAMETOOLONG;
        return ErrorCode::kInvalidArgument;
    }

    UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }

    ::unlink(cpath);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, cpath, std::strlen(cpath));

    if (::bind(sfd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }
    if (::listen(sfd.Get(), backlog) < 0)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }

    *out = std::move(sfd);
    return ErrorCode::kOk;
}

/**
 * @brief 连接到指定路径的 Unix domain socket
 * @param path       socket 文件路径
 * @param out        成功时写入已连接的 socket
 * @param sys_errno  可选：失败时的 errno
 */
[[nodiscard]]
inline ErrorCode ConnectUnixSocket(std::string_view path, UniqueFd* out, int* sys_errno = nullptr) noexcept
{
    if (!out)
        return ErrorCode::kInvalidArgument;

    char cpath[sizeof(sockaddr_un::sun_path)];
    if (!CopyUnixPath(path, cpath, sizeof(cpath)))
    {
        if (sys_errno)
            *sys_errno = ENAMETOOLONG;
        return ErrorCode::kInvalidArgument;
    }

    UniqueFd sfd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (!sfd)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, cpath, std::strlen(cpath));

    if (::connect(sfd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        if (sys_errno)
            *sys_errno = errno;
        return ErrnoToErrorCode(errno);
    }

    *out = std::move(sfd);
    return ErrorCode::kOk;
}

/**
 * @brief 删除 Unix socket 路径（栈缓冲拷贝，不分配内存）
 * @return true 成功或路径本就不存在；false 路径非法或 unlink 失败
 */
inline bool UnlinkUnixSocket(std::string_view path) noexcept
{
    char cpath[sizeof(sockaddr_un::sun_path)];
    if (!CopyUnixPath(path, cpath, sizeof(cpath)))
        return false;
    return ::unlink(cpath) == 0 || errno == ENOENT;
}

}  // namespace shm

#endif  // SHM_IPC_COMMON_HPP_
