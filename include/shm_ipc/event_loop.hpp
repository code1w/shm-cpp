// event_loop.hpp — poll + timerfd event loop (C++17)
//
// Provides a simple single-threaded event loop suitable for managing multiple
// clients, each with their own timer and socket fd.

#ifndef SHM_IPC_EVENT_LOOP_HPP
#define SHM_IPC_EVENT_LOOP_HPP

#include "common.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace shm_ipc {

class EventLoop {
public:
    using Callback = std::function<void(int fd, short revents)>;

    EventLoop() = default;
    ~EventLoop() = default;

    // Non-copyable, non-movable (callbacks may reference local state)
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    /// Register an fd for monitoring. `events` defaults to POLLIN.
    void add_fd(int fd, Callback cb, short events = POLLIN) {
        entries_.push_back({fd, events, std::move(cb)});
    }

    /// Remove an fd from monitoring. Safe to call from within a callback.
    void remove_fd(int fd) {
        pending_removals_.push_back(fd);
    }

    /// Create a repeating timerfd and register it. Returns the timerfd.
    /// The caller does NOT need to close the fd — EventLoop owns it.
    int add_timer(int interval_ms, Callback cb) {
        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (tfd < 0)
            throw std::runtime_error(std::string("timerfd_create: ") + std::strerror(errno));

        itimerspec ts{};
        ts.it_value.tv_sec  = interval_ms / 1000;
        ts.it_value.tv_nsec = (interval_ms % 1000) * 1000000L;
        ts.it_interval = ts.it_value;

        if (::timerfd_settime(tfd, 0, &ts, nullptr) < 0) {
            ::close(tfd);
            throw std::runtime_error(std::string("timerfd_settime: ") + std::strerror(errno));
        }

        owned_timerfds_.emplace_back(tfd);
        add_fd(tfd, std::move(cb));
        return tfd;
    }

    /// Remove a timer fd (stops the timer and closes the fd).
    void remove_timer(int tfd) {
        remove_fd(tfd);
        auto it = std::find_if(owned_timerfds_.begin(), owned_timerfds_.end(),
                               [tfd](const UniqueFd &u) { return u.get() == tfd; });
        if (it != owned_timerfds_.end())
            owned_timerfds_.erase(it);
    }

    /// Run the event loop until stop() is called. Poll timeout in ms (-1 = block).
    void run(int poll_timeout_ms = 500) {
        running_ = true;
        while (running_) {
            // Build pollfd array
            std::vector<pollfd> pfds;
            pfds.reserve(entries_.size());
            for (auto &e : entries_)
                pfds.push_back({e.fd, e.events, 0});

            int ret = ::poll(pfds.data(), pfds.size(), poll_timeout_ms);
            if (ret < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
            }

            // Dispatch events
            for (std::size_t i = 0; i < pfds.size() && running_; ++i) {
                if (pfds[i].revents != 0 && entries_[i].cb)
                    entries_[i].cb(pfds[i].fd, pfds[i].revents);
            }

            // Process deferred removals
            apply_pending_removals();
        }
    }

    void stop() { running_ = false; }

    /// Drain a timerfd (must be called from the timer callback to clear the event).
    static uint64_t drain_timerfd(int tfd) {
        uint64_t expirations = 0;
        ::read(tfd, &expirations, sizeof(expirations));
        return expirations;
    }

private:
    struct Entry {
        int      fd;
        short    events;
        Callback cb;
    };

    void apply_pending_removals() {
        for (int fd : pending_removals_) {
            entries_.erase(
                std::remove_if(entries_.begin(), entries_.end(),
                               [fd](const Entry &e) { return e.fd == fd; }),
                entries_.end());
        }
        pending_removals_.clear();
    }

    std::vector<Entry>    entries_;
    std::vector<int>      pending_removals_;
    std::vector<UniqueFd> owned_timerfds_;
    bool                  running_ = false;
};

} // namespace shm_ipc

#endif // SHM_IPC_EVENT_LOOP_HPP
