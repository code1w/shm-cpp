/**
 * @file event_loop.hpp
 * @brief 基于 poll + timerfd 的事件循环（C++17）
 *
 * 提供简单的单线程事件循环，适用于管理多客户端场景，
 * 每个客户端拥有独立的定时器和 socket fd。
 */

#ifndef SHM_IPC_EVENT_LOOP_HPP_
#define SHM_IPC_EVENT_LOOP_HPP_

#include "common.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace shm_ipc {

/** @brief 基于 poll + timerfd 的单线程事件循环 */
class EventLoop
{
 public:
    using Callback = std::function<void(int fd, short revents)>;

    EventLoop()  = default;
    ~EventLoop() = default;

    // 禁止复制和移动（回调可能引用外部局部状态）
    EventLoop(const EventLoop &)            = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    /**
     * @brief 注册一个 fd 进行监听
     * @param fd     待监听的文件描述符
     * @param cb     事件触发时的回调
     * @param events 监听的事件掩码，默认 POLLIN
     */
    void AddFd(int fd, Callback cb, short events = POLLIN)
    {
        if (dispatching_)
            pending_additions_.push_back({fd, events, std::move(cb)});
        else
            entries_.push_back({fd, events, std::move(cb)});
    }

    /**
     * @brief 从监听列表中移除一个 fd（可在回调内安全调用）
     * @param fd 待移除的文件描述符
     */
    void RemoveFd(int fd) { pending_removals_.push_back(fd); }

    /**
     * @brief 创建重复触发的 timerfd 并注册到事件循环
     *
     * 调用方无需自行关闭 fd，EventLoop 持有其所有权。
     * @param interval_ms 定时间隔（毫秒）
     * @param cb          定时器触发时的回调
     * @return timerfd 文件描述符
     */
    int AddTimer(int interval_ms, Callback cb)
    {
        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd < 0)
            throw std::runtime_error(std::string("timerfd_create: ") +
                                     std::strerror(errno));

        itimerspec ts{};
        ts.it_value.tv_sec  = interval_ms / 1000;
        ts.it_value.tv_nsec = (interval_ms % 1000) * 1000000L;
        ts.it_interval      = ts.it_value;

        if (::timerfd_settime(tfd, 0, &ts, nullptr) < 0)
        {
            ::close(tfd);
            throw std::runtime_error(std::string("timerfd_settime: ") +
                                     std::strerror(errno));
        }

        owned_timerfds_.emplace_back(tfd);
        AddFd(tfd, std::move(cb));
        return tfd;
    }

    /**
     * @brief 移除定时器 fd（停止定时器并关闭 fd）
     * @param tfd AddTimer 返回的 timerfd
     */
    void RemoveTimer(int tfd)
    {
        RemoveFd(tfd);
        auto it = std::find_if(owned_timerfds_.begin(), owned_timerfds_.end(),
                               [tfd](const UniqueFd &u) { return u.Get() == tfd; });
        if (it != owned_timerfds_.end())
            owned_timerfds_.erase(it);
    }

    /**
     * @brief 运行事件循环，直到 Stop() 被调用
     * @param poll_timeout_ms poll 超时（毫秒），-1 表示阻塞等待
     */
    void Run(int poll_timeout_ms = 500)
    {
        running_.store(true, std::memory_order_relaxed);
        while (running_.load(std::memory_order_relaxed))
        {
            // 构建 pollfd 数组
            std::vector<pollfd> pfds;
            pfds.reserve(entries_.size());
            for (auto &e : entries_)
                pfds.push_back({e.fd, e.events, 0});

            int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()),
                             poll_timeout_ms);
            if (ret < 0)
            {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
            }

            // 分发事件
            dispatching_ = true;
            for (std::size_t i = 0; i < pfds.size() && running_.load(std::memory_order_relaxed); ++i)
            {
                if (pfds[i].revents != 0 && entries_[i].cb)
                    entries_[i].cb(pfds[i].fd, pfds[i].revents);
            }
            dispatching_ = false;

            // 处理延迟添加和移除
            ApplyPendingAdditions();
            ApplyPendingRemovals();
        }
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
        ::read(tfd, &expirations, sizeof(expirations));
        return expirations;
    }

 private:
    /** @brief fd 监听条目 */
    struct Entry
    {
        int fd;        ///< 文件描述符
        short events;  ///< 监听事件掩码
        Callback cb;   ///< 事件回调
    };

    /** @brief 处理所有待移除的 fd */
    void ApplyPendingRemovals()
    {
        for (int fd : pending_removals_)
        {
            entries_.erase(
                std::remove_if(entries_.begin(), entries_.end(),
                               [fd](const Entry &e) { return e.fd == fd; }),
                entries_.end());
        }
        pending_removals_.clear();
    }

    /** @brief 处理所有待添加的 fd（回调分发期间延迟添加） */
    void ApplyPendingAdditions()
    {
        for (auto &e : pending_additions_)
            entries_.push_back(std::move(e));
        pending_additions_.clear();
    }

    std::vector<Entry> entries_;            ///< 当前监听的 fd 条目列表
    std::vector<int> pending_removals_;     ///< 待移除的 fd 列表（延迟删除）
    std::vector<Entry> pending_additions_;  ///< 待添加的 fd 条目（延迟添加）
    std::vector<UniqueFd> owned_timerfds_;  ///< EventLoop 持有的 timerfd 列表
    std::atomic<bool> running_{false};      ///< 事件循环运行标志（atomic 以支持信号处理器写入）
    bool dispatching_ = false;              ///< 正在分发回调标志
};

}  // namespace shm_ipc

#endif  // SHM_IPC_EVENT_LOOP_HPP_
