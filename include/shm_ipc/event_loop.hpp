/**
 * @file event_loop.hpp
 * @brief 基于 poll + timerfd 的事件循环（C++17）
 *
 * 提供简单的单线程事件循环，适用于管理多客户端场景，
 * 每个客户端拥有独立的定时器和 socket fd。
 *
 * 增删语义（墓碑机制）：
 * - AddFd 立即追加条目；分发期间调用时新条目自下一轮 poll 生效
 *   （本轮 pollfd 数组在分发前已快照完成）
 * - RemoveFd / RemoveTimer 立即将匹配条目置为墓碑（dead）：本轮后续
 *   分发不再触发该条目（包括 poll 时已捕获的 revents），条目在本轮
 *   分发结束后统一回收。移除作用于"调用时已存在的条目"而非裸 fd 号，
 *   因此同一轮内"先 RemoveFd 后 AddFd（fd 号复用）"不会误删新条目，
 *   "先 AddFd 后 RemoveFd"则能正确移除刚注册的条目。
 */

#ifndef SHM_IPC_EVENT_LOOP_HPP_
#define SHM_IPC_EVENT_LOOP_HPP_

#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <new>
#include <utility>
#include <vector>

#include "common.hpp"
#include "error.hpp"

namespace shm {

/** @brief 基于 poll + timerfd 的单线程事件循环 */
class EventLoop
{
 public:
    using Callback = std::function<void(int fd, short revents)>;
    using Hook    = std::function<void()>;
    using HookId  = uint64_t;

    EventLoop()  = default;
    ~EventLoop() = default;

    // 禁止复制和移动（回调可能引用外部局部状态）
    EventLoop(const EventLoop &)            = delete;
    EventLoop &operator=(const EventLoop &) = delete;
    EventLoop(EventLoop &&)                 = delete;
    EventLoop &operator=(EventLoop &&)      = delete;

    /**
     * @brief 注册一个 fd 进行监听
     * @param fd     待监听的文件描述符
     * @param cb     事件触发时的回调
     * @param events 监听的事件掩码，默认 POLLIN
     *
     * 可在回调内安全调用：新条目自下一轮 poll 生效。
     */
    void AddFd(int fd, Callback cb, short events = POLLIN)
    {
        entries_.push_back({fd, events, false, std::move(cb)});
    }

    /**
     * @brief 注册"本轮分发结束后"钩子（用于合并通知等延迟动作）
     *
     * 钩子在每次 poll 分发批次结束后、重新进入 poll 之前执行；Stop() 后
     * 的最后一个批次仍会执行。钩子不得抛异常、不得在自身执行期间
     * Add/Remove 钩子。
     *
     * @param hook 钩子函数
     * @param out  成功时写入钩子 ID
     * @return kOk 成功；kInvalidArgument 参数为空；kNoMemory 分配失败
     */
    [[nodiscard]]
    ErrorCode AddPostDispatchHook(Hook hook, HookId *out)
    {
        if (!out || !hook)
            return ErrorCode::kInvalidArgument;
        HookId id = next_hook_id_++;
        try
        {
            post_hooks_.push_back({id, std::move(hook)});
        }
        catch (const std::bad_alloc &)
        {
            return ErrorCode::kNoMemory;
        }
        *out = id;
        return ErrorCode::kOk;
    }

    /** @brief 移除 post-dispatch 钩子（id 为 0 时忽略） */
    void RemovePostDispatchHook(HookId id)
    {
        if (id == 0)
            return;
        auto it = std::find_if(post_hooks_.begin(), post_hooks_.end(),
                               [id](const auto &p) { return p.first == id; });
        if (it != post_hooks_.end())
            post_hooks_.erase(it);
    }

    /**
     * @brief 从监听列表中移除一个 fd（可在回调内安全调用）
     *
     * 立即将所有匹配的存活条目置为墓碑：本轮后续分发不再触发该 fd
     * （即使 poll 时其 revents 非零），条目在本轮分发结束后回收。
     * 若同一轮内该 fd 号被新连接复用并重新 AddFd，新条目不受影响。
     *
     * @param fd 待移除的文件描述符
     */
    void RemoveFd(int fd)
    {
        for (auto &e : entries_)
            if (e.fd == fd)
                e.dead = true;
    }

    /**
     * @brief 创建重复触发的 timerfd 并注册到事件循环
     *
     * 调用方无需自行关闭 fd，EventLoop 持有其所有权。
     * @param interval_ms 定时间隔（毫秒）
     * @param cb          定时器触发时的回调
     * @param out_tfd     成功时写入 timerfd
     * @param sys_errno   可选：失败时的 errno
     * @return kOk 成功；kNoMemory 回调注册分配失败；其余见 ErrorCode
     */
    [[nodiscard]]
    ErrorCode AddTimer(int interval_ms, Callback cb, int *out_tfd,
                       int *sys_errno = nullptr)
    {
        if (!out_tfd)
            return ErrorCode::kInvalidArgument;

        int tfd = -1;
        ErrorCode ec = CreateTimerfd(interval_ms, interval_ms, &tfd, sys_errno);
        if (ec != ErrorCode::kOk)
            return ec;

        try
        {
            owned_timerfds_.emplace_back(tfd);
        }
        catch (const std::bad_alloc &)
        {
            ::close(tfd);  // 尚未被 UniqueFd 接管
            return ErrorCode::kNoMemory;
        }
        try
        {
            AddFd(tfd, std::move(cb));
        }
        catch (const std::bad_alloc &)
        {
            CloseOwnedTimer(tfd);
            return ErrorCode::kNoMemory;
        }

        *out_tfd = tfd;
        return ErrorCode::kOk;
    }

    /**
     * @brief 创建一次性定时器，触发后自动从事件循环移除并关闭 fd
     *
     * @param delay_ms  延迟（毫秒），<=0 视为立即触发
     * @param cb        触发时的回调
     * @param out_tfd   成功时写入 timerfd（触发前可用 RemoveTimer 取消，
     *                  取消后回调保证不再触发）
     * @param sys_errno 可选：失败时的 errno
     */
    [[nodiscard]]
    ErrorCode AddOneShotTimer(int delay_ms, Callback cb, int *out_tfd,
                              int *sys_errno = nullptr)
    {
        if (!out_tfd)
            return ErrorCode::kInvalidArgument;

        int tfd = -1;
        ErrorCode ec = CreateTimerfd(delay_ms, 0, &tfd, sys_errno);
        if (ec != ErrorCode::kOk)
            return ec;

        try
        {
            owned_timerfds_.emplace_back(tfd);
        }
        catch (const std::bad_alloc &)
        {
            ::close(tfd);  // 尚未被 UniqueFd 接管
            return ErrorCode::kNoMemory;
        }

        try
        {
            AddFd(tfd, [this, tfd, cb = std::move(cb)](int fd, short revents) {
                DrainTimerfd(fd);
                // 先打墓碑（本轮不再补触发），但延迟 close：回调拿到的 fd
                // 仍有效（旧实现先 RemoveTimer 关闭 fd 再回调，回调内
                // fstat/read 该 fd 会 EBADF）
                RemoveFd(tfd);
                try
                {
                    cb(fd, revents);
                }
                catch (...)
                {
                    CloseOwnedTimer(tfd);
                    throw;
                }
                CloseOwnedTimer(tfd);
            });
        }
        catch (const std::bad_alloc &)
        {
            CloseOwnedTimer(tfd);
            return ErrorCode::kNoMemory;
        }

        *out_tfd = tfd;
        return ErrorCode::kOk;
    }

    /**
     * @brief 移除定时器 fd（停止定时器并关闭 fd）
     *
     * 若在本轮分发内调用（如验证成功后取消超时定时器），该定时器
     * 本轮不会再被补触发。
     * @param tfd AddTimer 返回的 timerfd
     */
    void RemoveTimer(int tfd)
    {
        // 先打墓碑（立即停止分发，含本轮尚未分发的旧 revents），再关闭 fd
        RemoveFd(tfd);
        CloseOwnedTimer(tfd);
    }

    /**
     * @brief 运行事件循环，直到 Stop() 被调用
     * @param poll_timeout_ms poll 超时（毫秒），-1 表示阻塞等待
     * @param sys_errno       可选：poll 失败时的 errno
     * @return kOk 正常停止；kNoMemory 构建 pollfd 数组失败；
     *         kSystem/kTimeout poll 失败；用户回调异常仍向外传播
     */
    [[nodiscard]]
    ErrorCode Run(int poll_timeout_ms = 500, int *sys_errno = nullptr)
    {
        running_.store(true, std::memory_order_relaxed);
        while (running_.load(std::memory_order_relaxed))
        {
            // 构建 pollfd 数组（跳过墓碑条目；idx 记录 pfds → entries 下标映射）
            std::vector<pollfd> pfds;
            std::vector<std::size_t> idx;
            try
            {
                pfds.reserve(entries_.size());
                idx.reserve(entries_.size());
                for (std::size_t k = 0; k < entries_.size(); ++k)
                {
                    if (entries_[k].dead)
                        continue;
                    pfds.push_back({entries_[k].fd, entries_[k].events, 0});
                    idx.push_back(k);
                }
            }
            catch (const std::bad_alloc &)
            {
                running_.store(false, std::memory_order_relaxed);
                return ErrorCode::kNoMemory;
            }

            int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()),
                             poll_timeout_ms);
            if (ret < 0)
            {
                if (errno == EINTR)
                    continue;
                if (sys_errno)
                    *sys_errno = errno;
                running_.store(false, std::memory_order_relaxed);
                return ErrnoToErrorCode(errno);
            }

            // 分发事件。分发期间 AddFd 只追加（deque 追加不失效既有元素引用）、
            // RemoveFd 只打墓碑（分发前重查），下标映射保持稳定
            try
            {
                for (std::size_t i = 0;
                     i < pfds.size() && running_.load(std::memory_order_relaxed); ++i)
                {
                    Entry &e = entries_[idx[i]];
                    if (!e.dead && pfds[i].revents != 0 && e.cb)
                        e.cb(pfds[i].fd, pfds[i].revents);
                }
            }
            catch (const std::bad_alloc &)
            {
                // 回调内部（如 AddFd）分配失败：先执行延迟钩子（如通知
                // 合并），再回收墓碑并转错误码
                RunPostDispatchHooksNoThrow();
                CompactDead();
                running_.store(false, std::memory_order_relaxed);
                return ErrorCode::kNoMemory;
            }
            catch (...)
            {
                // 用户回调抛异常：仍执行延迟钩子并回收墓碑，保持状态一致
                RunPostDispatchHooksNoThrow();
                CompactDead();
                throw;
            }

            // 本轮分发结束：执行延迟钩子（如合并后的对端通知）
            RunPostDispatchHooksNoThrow();
            // 回收墓碑条目
            CompactDead();
        }
        return ErrorCode::kOk;
    }

    /** @brief 停止事件循环（线程安全、信号安全） */
    void Stop() { running_.store(false, std::memory_order_relaxed); }

    /**
     * @brief 排空 timerfd（必须在定时器回调中调用以清除事件）
     * @param tfd timerfd 文件描述符
     * @return 到期次数
     */
    static uint64_t DrainTimerfd(int tfd)
    {
        uint64_t expirations = 0;
        ssize_t r;
        do {
            r = ::read(tfd, &expirations, sizeof(expirations));
        } while (r < 0 && errno == EINTR);
        return expirations;
    }

 private:
    /**
     * @brief 创建 timerfd（initial_ms 后首次触发，interval_ms 为 0 表示一次性）
     * @param out_fd 成功时写入 timerfd
     */
    [[nodiscard]]
    static ErrorCode CreateTimerfd(int initial_ms, int interval_ms,
                                   int *out_fd, int *sys_errno) noexcept
    {
        if (!out_fd)
            return ErrorCode::kInvalidArgument;

        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd < 0)
        {
            if (sys_errno)
                *sys_errno = errno;
            return ErrnoToErrorCode(errno);
        }

        itimerspec ts{};
        // it_value 全零会被内核解释为"解除定时"（永不触发）：
        // initial_ms <= 0 时钳为 1ns，保证零/负延迟定时器立即触发
        if (initial_ms > 0)
        {
            ts.it_value.tv_sec  = initial_ms / 1000;
            ts.it_value.tv_nsec = (initial_ms % 1000) * 1000000L;
        }
        else
        {
            ts.it_value.tv_nsec = 1;
        }
        // interval_ms <= 0 同理钳为 0（不重复）
        if (interval_ms > 0)
        {
            ts.it_interval.tv_sec  = interval_ms / 1000;
            ts.it_interval.tv_nsec = (interval_ms % 1000) * 1000000L;
        }

        if (::timerfd_settime(tfd, 0, &ts, nullptr) < 0)
        {
            const int err = errno;
            ::close(tfd);
            if (sys_errno)
                *sys_errno = err;
            return ErrnoToErrorCode(err);
        }
        *out_fd = tfd;
        return ErrorCode::kOk;
    }

    /** @brief fd 监听条目 */
    struct Entry
    {
        int fd;        ///< 文件描述符
        short events;  ///< 监听事件掩码
        bool dead;     ///< 墓碑标记：不再参与 poll 与分发，待回收
        Callback cb;   ///< 事件回调
    };

    /** @brief 执行 post-dispatch 钩子；钩子异常被吞掉以免破坏事件循环 */
    void RunPostDispatchHooksNoThrow() noexcept
    {
        try
        {
            for (auto &h : post_hooks_)
                h.second();
        }
        catch (...)
        {
            // 钩子应为内部通知逻辑，不应抛异常；吞掉以保证循环继续
        }
    }

    /** @brief 关闭并移除 EventLoop 持有的 timerfd（不处理 entries_） */
    void CloseOwnedTimer(int tfd)
    {
        auto it = std::find_if(owned_timerfds_.begin(), owned_timerfds_.end(),
                               [tfd](const UniqueFd &u) { return u.Get() == tfd; });
        if (it != owned_timerfds_.end())
            owned_timerfds_.erase(it);
    }

    /** @brief 回收所有墓碑条目（仅在非分发阶段调用） */
    void CompactDead()
    {
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [](const Entry &e) { return e.dead; }),
            entries_.end());
    }

    // deque 而非 vector：回调内 AddFd 追加时，既有元素的引用不失效，
    // 分发循环中持有的 Entry& 在回调执行期间始终有效
    std::deque<Entry> entries_;           ///< fd 监听条目列表
    std::vector<UniqueFd> owned_timerfds_;  ///< EventLoop 持有的 timerfd 列表
    std::vector<std::pair<HookId, Hook>> post_hooks_;  ///< 分发批次结束钩子
    HookId next_hook_id_ = 1;               ///< 下一个钩子 ID
    std::atomic<bool> running_{false};      ///< 事件循环运行标志（atomic 以支持信号处理器写入）
};

}  // namespace shm

#endif  // SHM_IPC_EVENT_LOOP_HPP_
