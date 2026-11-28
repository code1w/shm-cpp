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
#include <limits>
#include <utility>
#include <type_traits>
#include <vector>

#include "common.hpp"
#include "error.hpp"

namespace shm {

/** @brief 基于 poll + timerfd 的单线程事件循环 */
class EventLoop
{
 public:
    using Callback = std::function<void(int fd, short revents)>;
    using Hook     = std::function<void()>;
    using HookId   = uint64_t;

    // A registration owns only a subscription. Watched timer subscriptions also
    // own their timerfd; ordinary watched fds remain owned by the caller.
    // All operations are owner-thread only. The EventLoop must outlive the handle.
    class Registration
    {
        friend class EventLoop;
        EventLoop* loop_ = nullptr;
        uint64_t id_     = 0;
        Registration(EventLoop* loop, uint64_t id) noexcept : loop_(loop), id_(id) {}

     public:
        Registration() noexcept = default;
        ~Registration() { Reset(); }
        Registration(const Registration&)            = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept
            : loop_(std::exchange(other.loop_, nullptr)), id_(std::exchange(other.id_, 0))
        {}
        Registration& operator=(Registration&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                loop_ = std::exchange(other.loop_, nullptr);
                id_   = std::exchange(other.id_, 0);
            }
            return *this;
        }
        void Reset() noexcept;
        uint64_t Id() const noexcept { return id_; }
        int Fd() const noexcept;
        explicit operator bool() const noexcept;
    };

    // The callable is stored directly in one nothrow-allocated node. No
    // std::function conversion/allocation is required by these new interfaces.
    // Failure leaves the existing output registration unchanged.
    template <class F>
    [[nodiscard]]
    ErrorCode WatchFd(int fd, F&& cb, Registration* out, short events = POLLIN) noexcept
    {
        using Stored = std::decay_t<F>;
        CheckWatchCallable<Stored, F>();
        static_assert(std::is_same_v<std::invoke_result_t<Stored&, int, short>, void>, "fd callback must return void");
        if (!out || fd < 0 || !ValidCallable(cb) || !next_registration_id_)
            return ErrorCode::kInvalidArgument;
        auto* entry = new (std::nothrow) WatchedNode<Stored>(fd, events, std::forward<F>(cb));
        if (!entry)
            return ErrorCode::kNoMemory;
        InstallWatch(entry, out);
        return ErrorCode::kOk;
    }

    template <class F>
    [[nodiscard]]
    ErrorCode WatchBusyFd(int fd, F&& cb, Registration* out) noexcept
    {
        using Stored = std::decay_t<F>;
        CheckWatchCallable<Stored, F>();
        static_assert(std::is_invocable_r_v<ErrorCode, Stored&, int, short>, "busy callback must return ErrorCode");
        if (!out || !busy_polling_ || fd < 0 || !ValidCallable(cb) || !next_registration_id_)
            return ErrorCode::kInvalidArgument;
        auto* entry = new (std::nothrow) BusyNode<Stored>(fd, std::forward<F>(cb));
        if (!entry)
            return ErrorCode::kNoMemory;
        entry->registration_id = next_registration_id_++;
        if (busy_tail_)
            busy_tail_->next = entry;
        else
            busy_head_ = entry;
        busy_tail_ = entry;
        *out       = Registration(this, entry->registration_id);
        return ErrorCode::kOk;
    }

    template <class F>
    [[nodiscard]]
    ErrorCode WatchTimer(int interval_ms, F&& cb, Registration* out, int* sys_errno = nullptr) noexcept
    {
        if (interval_ms <= 0)
            return ErrorCode::kInvalidArgument;
        return WatchTimed(interval_ms, interval_ms, std::forward<F>(cb), out, sys_errno);
    }
    template <class F>
    [[nodiscard]]
    ErrorCode WatchOneShotTimer(int64_t delay_ms, F&& cb, Registration* out, int* sys_errno = nullptr) noexcept
    {
        return WatchTimed(delay_ms, 0, std::forward<F>(cb), out, sys_errno);
    }

    // Exact identity cancellation; a stale handle cannot cancel a reused fd.
    void CancelRegistration(uint64_t id) noexcept
    {
        if (!id)
            return;
        BusyEntry** busy_link    = &busy_head_;
        BusyEntry* busy_previous = nullptr;
        while (*busy_link)
        {
            BusyEntry* e = *busy_link;
            if (e->registration_id == id)
            {
                e->dead = true;
                if (!run_active_.load(std::memory_order_relaxed) && !destroying_ && !compacting_)
                {
                    *busy_link = e->next;
                    if (busy_tail_ == e)
                        busy_tail_ = busy_previous;
                    delete e;
                }
                return;
            }
            busy_previous = e;
            busy_link     = &e->next;
        }
        WatchedEntry** link    = &watched_head_;
        WatchedEntry* previous = nullptr;
        while (*link)
        {
            WatchedEntry* e = *link;
            if (e->id == id)
            {
                e->dead = true;
                // Preserve the timerfd until its current callback returns.
                if (!e->dispatching)
                    e->timer_fd.Reset();
                if (!run_active_.load(std::memory_order_relaxed) && !destroying_ && !compacting_)
                {
                    *link = e->next;
                    if (watched_tail_ == e)
                        watched_tail_ = previous;
                    --watched_count_;
                    delete e;
                }
                return;
            }
            previous = e;
            link     = &e->next;
        }
    }

    EventLoop() { wake_error_ = CreateEventfd(&wake_fd_, &wake_errno_); }
    ~EventLoop()
    {
        destroying_ = true;
        while (watched_head_)
        {
            WatchedEntry* old = watched_head_;
            watched_head_     = old->next;
            delete old;
        }
        while (busy_head_)
        {
            BusyEntry* old = busy_head_;
            busy_head_ = old->next;
            delete old;
        }
    }

    // 禁止复制和移动（回调可能引用外部局部状态）
    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&)                 = delete;
    EventLoop& operator=(EventLoop&&)      = delete;

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
        entries_.push_back({next_registration_id_++, fd, events, false, std::move(cb)});
    }

    /// Configure on the owner thread before registering fds/connecting/Run.
    /// Control fds are checked every control_interval spin iterations, not a time bound.
    [[nodiscard]]
    ErrorCode SetBusyPolling(bool enabled, uint32_t control_interval = 256) noexcept
    {
        if (run_active_.load(std::memory_order_relaxed) || !entries_.empty() || busy_head_ || watched_head_ ||
            !control_interval)
            return ErrorCode::kInvalidArgument;
        busy_polling_ = enabled;
        control_interval_ = control_interval;
        return ErrorCode::kOk;
    }
    bool BusyPolling() const noexcept { return busy_polling_; }
    using PollCallback = ErrorCode (*)(int, short);
    /// Callback must not throw; failures propagate from Run as an ErrorCode.
    /// Same removal rules as AddFd; receives -1, 0 (no fd read).
    template<class F>
    [[nodiscard]]
    ErrorCode AddBusyPollFd(int fd, F&& cb) noexcept
    {
        using Stored = std::decay_t<F>;
        static_assert(std::is_nothrow_constructible_v<Stored, F&&>, "callback storage must not throw");
        static_assert(std::is_invocable_r_v<ErrorCode, Stored&, int, short>, "callback must return ErrorCode");
        if (!busy_polling_ || fd < 0) return ErrorCode::kInvalidArgument;
        if constexpr (std::is_pointer_v<Stored>)
            if (!cb) return ErrorCode::kInvalidArgument;
        // Store the callable directly; no std::function/container allocation.
        BusyEntry* entry = new (std::nothrow) BusyNode<Stored>(fd, std::forward<F>(cb));
        if (!entry) return ErrorCode::kNoMemory;
        if (busy_tail_) busy_tail_->next = entry;
        else busy_head_ = entry;
        busy_tail_ = entry;
        return ErrorCode::kOk;
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
    ErrorCode AddPostDispatchHook(Hook hook, HookId* out)
    {
        if (!out || !hook)
        {
            return ErrorCode::kInvalidArgument;
        }
        const HookId id = next_hook_id_++;
        try
        {
            post_hooks_.push_back({id, std::move(hook)});
        }
        catch (const std::bad_alloc&)
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
        {
            return;
        }
        auto it = std::find_if(post_hooks_.begin(), post_hooks_.end(), [id](const auto& p) { return p.first == id; });
        if (it != post_hooks_.end())
        {
            post_hooks_.erase(it);
        }
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
        for (WatchedEntry* e = watched_head_; e; e = e->next)
            if (e->fd == fd)
            {
                e->dead = true;
                if (!e->dispatching)
                    e->timer_fd.Reset();
            }
        for (BusyEntry* e = busy_head_; e; e = e->next)
            if (e->fd == fd) e->dead = true;

        for (auto& e : entries_)
        {
            if (e.fd == fd)
            {
                e.dead = true;
            }
        }
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
    ErrorCode AddTimer(int interval_ms, Callback cb, int* out_tfd, int* sys_errno = nullptr)
    {
        if (!out_tfd)
        {
            return ErrorCode::kInvalidArgument;
        }

        int tfd            = -1;
        const ErrorCode ec = CreateTimerfd(interval_ms, interval_ms, &tfd, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            return ec;
        }

        try
        {
            owned_timerfds_.emplace_back(tfd, next_timer_id_++);
        }
        catch (const std::bad_alloc&)
        {
            ::close(tfd);  // 尚未被 UniqueFd 接管
            return ErrorCode::kNoMemory;
        }
        try
        {
            AddFd(tfd, std::move(cb));
        }
        catch (const std::bad_alloc&)
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
    ErrorCode AddOneShotTimer(int64_t delay_ms, Callback cb, int* out_tfd, int* sys_errno = nullptr)
    {
        if (!out_tfd)
        {
            return ErrorCode::kInvalidArgument;
        }

        int tfd            = -1;
        const ErrorCode ec = CreateTimerfd(delay_ms, 0, &tfd, sys_errno);
        if (ec != ErrorCode::kOk)
        {
            return ec;
        }

        try
        {
            owned_timerfds_.emplace_back(tfd, next_timer_id_++);
        }
        catch (const std::bad_alloc&)
        {
            ::close(tfd);  // 尚未被 UniqueFd 接管
            return ErrorCode::kNoMemory;
        }

        try
        {
            const uint64_t timer_id = owned_timerfds_.back().id;
            AddFd(tfd, [this, tfd, timer_id, cb = std::move(cb)](int fd, short revents) {
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
                    CloseOwnedTimerById(timer_id);
                    throw;
                }
                CloseOwnedTimerById(timer_id);
            });
        }
        catch (const std::bad_alloc&)
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
     * @brief 运行事件循环，直到 Stop() 被调用；同一实例不允许嵌套或并发 Run
     * @param poll_timeout_ms poll 超时（毫秒），-1 表示阻塞等待
     * @param sys_errno       可选：poll 失败时的 errno
     * @return kInvalidArgument 已有 Run 尚未退出（errno 出参为 EBUSY）；
     *         kOk 正常停止；kNoMemory 构建 pollfd 数组失败；
     *         kSystem/kTimeout poll 失败；用户回调异常仍向外传播
     */
    [[nodiscard]]
    ErrorCode Run(int poll_timeout_ms = 500, int* sys_errno = nullptr)
    {
        // Stop 只请求退出，不能让仍在分发/回收的外层 Run 提前失去所有权。
        bool expected = false;
        if (!run_active_.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                 std::memory_order_relaxed))
        {
            if (sys_errno) *sys_errno = EBUSY;
            return ErrorCode::kInvalidArgument;
        }
        struct RunGuard
        {
            std::atomic<bool>& active;
            std::atomic<bool>& running;
            ~RunGuard()
            {
                running.store(false, std::memory_order_relaxed);
                active.store(false, std::memory_order_release);
            }
        } guard{run_active_, running_};

        if (wake_error_ != ErrorCode::kOk)
        {
            if (sys_errno) *sys_errno = wake_errno_;
            return wake_error_;
        }
        running_.store(true, std::memory_order_relaxed);
        uint32_t spins = 0;
        while (running_.load(std::memory_order_relaxed))
        {
            if (busy_polling_)
            {
                BusyEntry* end = busy_tail_; // additions become active next spin
                ErrorCode result = ErrorCode::kOk;
                for (BusyEntry* e = busy_head_; e && running_.load(std::memory_order_relaxed); e = e->next)
                {
                    if (!e->dead) result = e->invoke(e, -1, 0);
                    if (result != ErrorCode::kOk || e == end) break;
                }
                RunPostDispatchHooksNoThrow();
                CompactDead();
                if (result != ErrorCode::kOk) return result;
                if (!running_.load(std::memory_order_relaxed)) break;
                if (++spins < control_interval_)
                {
                    continue;
                }
                spins = 0;
            }
            // 构建 pollfd 与稳定分发引用快照（跳过墓碑条目）
            std::vector<pollfd> pfds;
            std::vector<DispatchTarget> targets;
            try
            {
                pfds.reserve(entries_.size() + watched_count_ + 1);
                targets.reserve(entries_.size() + watched_count_);
                // Merge by registration order, preserving ordering across old
                // AddFd and scoped WatchFd interfaces. Nodes/references stay
                // stable during callbacks; cancellation only marks tombstones.
                auto legacy           = entries_.begin();
                WatchedEntry* watched = watched_head_;
                while (legacy != entries_.end() || watched)
                {
                    if (watched && (legacy == entries_.end() || watched->id < legacy->id))
                    {
                        if (!watched->dead)
                        {
                            pfds.push_back({watched->fd, watched->events, 0});
                            targets.push_back({nullptr, watched});
                        }
                        watched = watched->next;
                    }
                    else
                    {
                        if (!legacy->dead)
                        {
                            pfds.push_back({legacy->fd, legacy->events, 0});
                            targets.push_back({&*legacy, nullptr});
                        }
                        ++legacy;
                    }
                }
                pfds.push_back({wake_fd_.Get(), POLLIN, 0});
            }
            catch (const std::bad_alloc&)
            {
                running_.store(false, std::memory_order_relaxed);
                return ErrorCode::kNoMemory;
            }

            const int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), busy_polling_ ? 0 : poll_timeout_ms);
            if (ret < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                if (sys_errno)
                {
                    *sys_errno = errno;
                }
                running_.store(false, std::memory_order_relaxed);
                return ErrnoToErrorCode(errno);
            }

            if (pfds.back().revents & POLLIN)
            {
                uint64_t count;
                while (::read(wake_fd_.Get(), &count, sizeof(count)) < 0 && errno == EINTR) {}
            }

            // 分发期间注册只追加，取消只打墓碑，快照中的引用保持有效。
            try
            {
                for (std::size_t i = 0; i < targets.size() && running_.load(std::memory_order_relaxed); ++i)
                {
                    if (!pfds[i].revents)
                        continue;
                    auto& target = targets[i];
                    if (target.watched)
                    {
                        WatchedEntry* e = target.watched;
                        if (e->dead)
                            continue;
                        if (e->timer_fd)
                        {
                            e->dispatching = true;
                            if (e->one_shot)
                            {
                                DrainTimerfd(e->fd);
                                e->dead = true;
                            }
                            WatchDispatchGuard guard{e};
                            e->invoke(e, pfds[i].fd, pfds[i].revents);
                        }
                        else e->invoke(e, pfds[i].fd, pfds[i].revents);
                    }
                    else
                    {
                        Entry& e = *target.legacy;
                        if (!e.dead && e.cb)
                            e.cb(pfds[i].fd, pfds[i].revents);
                    }
                }
            }
            catch (const std::bad_alloc&)
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
    void Stop() noexcept
    {
        const int saved_errno = errno;
        running_.store(false, std::memory_order_relaxed);
        const uint64_t one = 1;
        if (wake_fd_)
        {
            while (::write(wake_fd_.Get(), &one, sizeof(one)) < 0 && errno == EINTR) {}
        }
        errno = saved_errno;
    }

    /**
     * @brief 非阻塞排空 timerfd：周期回调需调用；一次性回调已由包装层排空
     * @param tfd timerfd 文件描述符
     * @return 到期次数，已排空时返回 0
     */
    static uint64_t DrainTimerfd(int tfd)
    {
        uint64_t expirations = 0;
        ssize_t r            = 0;
        do
        {
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
    static ErrorCode CreateTimerfd(int64_t initial_ms, int64_t interval_ms, int* out_fd, int* sys_errno) noexcept
    {
        if (!out_fd)
        {
            return ErrorCode::kInvalidArgument;
        }

        if (initial_ms / 1000 > std::numeric_limits<time_t>::max() ||
            interval_ms / 1000 > std::numeric_limits<time_t>::max())
        {
            return ErrorCode::kInvalidArgument;
        }

        const int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (tfd < 0)
        {
            if (sys_errno)
            {
                *sys_errno = errno;
            }
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
            {
                *sys_errno = err;
            }
            return ErrnoToErrorCode(err);
        }
        *out_fd = tfd;
        return ErrorCode::kOk;
    }

    /** @brief fd 监听条目 */
    struct BusyEntry
    {
        int fd;
        uint64_t registration_id = 0;
        bool dead = false;
        BusyEntry* next = nullptr;
        ErrorCode (*invoke)(BusyEntry*, int, short);
        BusyEntry(int descriptor, ErrorCode (*dispatch)(BusyEntry*, int, short)) noexcept
            : fd(descriptor), invoke(dispatch) {}
        virtual ~BusyEntry() = default;
    };
    template<class F>
    struct BusyNode final : BusyEntry
    {
        F cb;
        template<class T>
        BusyNode(int fd, T&& callback) noexcept : BusyEntry(fd, Dispatch), cb(std::forward<T>(callback)) {}
        static ErrorCode Dispatch(BusyEntry* entry, int fd, short events)
        {
            return static_cast<BusyNode*>(entry)->cb(fd, events);
        }
    };
    struct Entry
    {
        uint64_t id;
        int fd;        ///< 文件描述符
        short events;  ///< 监听事件掩码
        bool dead;     ///< 墓碑标记：不再参与 poll 与分发，待回收
        Callback cb;   ///< 事件回调
    };

    struct WatchedEntry
    {
        int fd;
        short events;
        uint64_t id = 0;
        bool dead = false, one_shot = false, dispatching = false;
        WatchedEntry* next = nullptr;
        UniqueFd timer_fd;
        void (*invoke)(WatchedEntry*, int, short);
        WatchedEntry(int descriptor, short mask, void (*dispatch)(WatchedEntry*, int, short)) noexcept
            : fd(descriptor), events(mask), invoke(dispatch)
        {}
        virtual ~WatchedEntry() = default;
    };
    template <class F>
    struct WatchedNode final : WatchedEntry
    {
        F cb;
        template <class T>
        WatchedNode(int fd, short events, T&& callback) noexcept
            : WatchedEntry(fd, events, Dispatch), cb(std::forward<T>(callback))
        {}
        static void Dispatch(WatchedEntry* entry, int fd, short events)
        {
            static_cast<WatchedNode*>(entry)->cb(fd, events);
        }
    };
    struct DispatchTarget
    {
        Entry* legacy;
        WatchedEntry* watched;
    };
    struct WatchDispatchGuard
    {
        WatchedEntry* entry;
        ~WatchDispatchGuard()
        {
            entry->dispatching = false;
            if (entry->dead)
                entry->timer_fd.Reset();
        }
    };
    template <class Stored, class Input>
    static constexpr void CheckWatchCallable() noexcept
    {
        static_assert(std::is_nothrow_constructible_v<Stored, Input&&>,
                      "move callback or use a nothrow-copyable callable");
        static_assert(std::is_nothrow_destructible_v<Stored>, "callback destructor must not throw");
    }
    template <class F>
    static bool ValidCallable(const F& cb) noexcept
    {
        if constexpr (std::is_pointer_v<std::decay_t<F>> || std::is_same_v<std::decay_t<F>, Callback>)
            return static_cast<bool>(cb);
        else
            return true;
    }
    void InstallWatch(WatchedEntry* entry, Registration* out) noexcept
    {
        entry->id = next_registration_id_++;
        if (watched_tail_)
            watched_tail_->next = entry;
        else
            watched_head_ = entry;
        watched_tail_ = entry;
        ++watched_count_;
        *out = Registration(this, entry->id);
    }
    template <class F>
    ErrorCode WatchTimed(int64_t initial_ms, int64_t interval_ms, F&& cb, Registration* out, int* sys_errno) noexcept
    {
        using Stored = std::decay_t<F>;
        CheckWatchCallable<Stored, F>();
        static_assert(std::is_same_v<std::invoke_result_t<Stored&, int, short>, void>,
                      "timer callback must return void");
        if (!out || !ValidCallable(cb) || !next_registration_id_)
            return ErrorCode::kInvalidArgument;
        int fd        = -1;
        const auto ec = CreateTimerfd(initial_ms, interval_ms, &fd, sys_errno);
        if (ec != ErrorCode::kOk)
            return ec;
        UniqueFd owner(fd);
        auto* entry = new (std::nothrow) WatchedNode<Stored>(fd, POLLIN, std::forward<F>(cb));
        if (!entry)
            return ErrorCode::kNoMemory;
        entry->timer_fd = std::move(owner);
        entry->one_shot = interval_ms == 0;
        InstallWatch(entry, out);
        return ErrorCode::kOk;
    }
    int RegisteredFd(uint64_t id) const noexcept
    {
        if (!id)
            return -1;
        for (const auto* e = watched_head_; e; e = e->next)
            if (e->id == id)
                return e->dead ? -1 : e->fd;
        for (const auto* e = busy_head_; e; e = e->next)
            if (e->registration_id == id)
                return e->dead ? -1 : e->fd;
        return -1;
    }

    /** @brief 执行 post-dispatch 钩子；钩子异常被吞掉以免破坏事件循环 */
    void RunPostDispatchHooksNoThrow() noexcept
    {
        try
        {
            for (auto& h : post_hooks_)
            {
                h.second();
            }
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
                               [tfd](const OwnedTimer& timer) { return timer.fd.Get() == tfd; });
        if (it != owned_timerfds_.end())
        {
            owned_timerfds_.erase(it);
        }
    }

    void CloseOwnedTimerById(uint64_t id)
    {
        auto it = std::find_if(owned_timerfds_.begin(), owned_timerfds_.end(),
                              [id](const OwnedTimer& timer) { return timer.id == id; });
        if (it != owned_timerfds_.end()) owned_timerfds_.erase(it);
    }

    struct OwnedTimer
    {
        UniqueFd fd;
        uint64_t id;
        OwnedTimer(int descriptor, uint64_t identity) noexcept : fd(descriptor), id(identity) {}
    };

    /** @brief 回收所有墓碑条目（仅在非分发阶段调用） */
    void CompactDead()
    {
        struct CompactGuard
        {
            bool& active;
            explicit CompactGuard(bool& flag) noexcept : active(flag) { active = true; }
            ~CompactGuard() { active = false; }
        } guard(compacting_);
        WatchedEntry** watched_link = &watched_head_;
        watched_tail_               = nullptr;
        while (*watched_link)
        {
            auto* e = *watched_link;
            if (e->dead)
            {
                *watched_link = e->next;
                --watched_count_;
                delete e;
            }
            else
            {
                watched_tail_ = e;
                watched_link  = &e->next;
            }
        }
        BusyEntry** link = &busy_head_;
        busy_tail_ = nullptr;
        while (*link)
        {
            BusyEntry* e = *link;
            if (e->dead)
            {
                *link = e->next;
                delete e;
            }
            else
            {
                busy_tail_ = e;
                link = &e->next;
            }
        }

        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.dead; }),
                       entries_.end());
    }

    // deque 而非 vector：回调内 AddFd 追加时，既有元素的引用不失效，
    // 分发循环中持有的 Entry& 在回调执行期间始终有效
    std::deque<Entry> entries_;                        ///< fd 监听条目列表
    std::vector<OwnedTimer> owned_timerfds_;             ///< EventLoop 持有的 timerfd 列表
    std::vector<std::pair<HookId, Hook>> post_hooks_;  ///< 分发批次结束钩子
    WatchedEntry* watched_head_    = nullptr;
    WatchedEntry* watched_tail_    = nullptr;
    std::size_t watched_count_     = 0;
    uint64_t next_registration_id_ = 1;
    bool destroying_ = false, compacting_ = false;
    BusyEntry* busy_head_ = nullptr;
    BusyEntry* busy_tail_ = nullptr;
    bool busy_polling_ = false;
    uint32_t control_interval_ = 256;
    UniqueFd wake_fd_;
    ErrorCode wake_error_ = ErrorCode::kOk;
    int wake_errno_ = 0;
    uint64_t next_timer_id_ = 1;
    HookId next_hook_id_ = 1;                          ///< 下一个钩子 ID
    /// 事件循环运行标志（atomic 以支持信号处理器写入）；纯停止标志，不发布其他数据，故各处用 relaxed
    std::atomic<bool> running_{false};
    std::atomic<bool> run_active_{false};  ///< Run 独占所有权，至分发/回收完全退出才释放
};

inline void EventLoop::Registration::Reset() noexcept
{
    EventLoop* owner  = std::exchange(loop_, nullptr);
    const uint64_t id = std::exchange(id_, 0);
    if (owner)
        owner->CancelRegistration(id);
}
inline int EventLoop::Registration::Fd() const noexcept { return loop_ ? loop_->RegisteredFd(id_) : -1; }
inline EventLoop::Registration::operator bool() const noexcept { return Fd() >= 0; }

}  // namespace shm

#endif  // SHM_IPC_EVENT_LOOP_HPP_
