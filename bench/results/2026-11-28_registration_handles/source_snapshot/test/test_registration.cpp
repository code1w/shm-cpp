#include <shm_ipc/event_loop.hpp>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
using Loop = shm::EventLoop;
using E    = shm::ErrorCode;
#define CHECK(cond)                                                 \
    do                                                              \
    {                                                               \
        if (!(cond))                                                \
        {                                                           \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); \
            std::abort();                                           \
        }                                                           \
    } while (0)
static int fail_nothrow = 0;
extern "C" void* __real__ZnwmRKSt9nothrow_t(size_t, const std::nothrow_t&);
extern "C" void* __wrap__ZnwmRKSt9nothrow_t(size_t n, const std::nothrow_t& tag) noexcept
{
    if (fail_nothrow > 0)
    {
        --fail_nothrow;
        return nullptr;
    }
    return __real__ZnwmRKSt9nothrow_t(n, tag);
}
static shm::UniqueFd Event()
{
    shm::UniqueFd fd;
    CHECK(shm::CreateEventfd(&fd) == E::kOk);
    return fd;
}
static void Ready(int fd)
{
    const uint64_t one = 1;
    CHECK(::write(fd, &one, 8) == 8);
}
static void Drain(int fd)
{
    uint64_t n = 0;
    CHECK(::read(fd, &n, 8) == 8);
}
static size_t Fds()
{
    DIR* d = ::opendir("/proc/self/fd");
    CHECK(d);
    size_t n = 0;
    while (auto* e = ::readdir(d))
        if (e->d_name[0] != '.')
            ++n;
    ::closedir(d);
    return n;
}
struct Probe
{
    int* destroyed;
    explicit Probe(int* p) noexcept : destroyed(p) {}
    Probe(Probe&& p) noexcept : destroyed(std::exchange(p.destroyed, nullptr)) {}
    Probe(const Probe&) = delete;
    ~Probe()
    {
        if (destroyed)
            ++*destroyed;
    }
};
static void LifetimeMoveFailure()
{
    static_assert(!std::is_copy_constructible_v<Loop::Registration>);
    static_assert(std::is_nothrow_move_constructible_v<Loop::Registration>);
    static_assert(std::is_nothrow_move_assignable_v<Loop::Registration>);
    Loop loop;
    auto fd       = Event();
    int destroyed = 0;
    Loop::Registration watch;
    CHECK(loop.WatchFd(
              fd.Get(), [p = Probe(&destroyed)](int, short) {}, &watch) == E::kOk);
    const auto first = watch.Id();
    CHECK(first && watch && watch.Fd() == fd.Get());
    Loop::Registration moved(std::move(watch));
    CHECK(!watch && watch.Id() == 0 && moved.Id() == first);
    watch = std::move(moved);
    CHECK(!moved && watch.Id() == first);
    watch = std::move(watch);
    CHECK(watch.Id() == first && watch);
    CHECK(loop.WatchFd(
              -1, [](int, short) {}, &watch) == E::kInvalidArgument &&
          watch.Id() == first);
    CHECK(loop.WatchFd(
              fd.Get(), [](int, short) {}, nullptr) == E::kInvalidArgument);
    CHECK(loop.WatchFd(fd.Get(), static_cast<void (*)(int, short)>(nullptr), &watch) == E::kInvalidArgument);
    fail_nothrow = 1;
    CHECK(loop.WatchFd(
              fd.Get(), [](int, short) {}, &watch) == E::kNoMemory &&
          watch.Id() == first);
    const size_t before = Fds();
    fail_nothrow        = 1;
    CHECK(loop.WatchOneShotTimer(
              1, [](int, short) {}, &watch) == E::kNoMemory);
    CHECK(watch.Id() == first && Fds() == before);
    CHECK(loop.WatchTimer(
              0, [](int, short) {}, &watch) == E::kInvalidArgument);
    watch.Reset();
    CHECK(!watch && watch.Id() == 0 && destroyed == 1);
    watch.Reset();
    CHECK(destroyed == 1 && ::fcntl(fd.Get(), F_GETFD) >= 0);
    {
        Loop::Registration scoped;
        CHECK(loop.WatchFd(
                  fd.Get(), [p = Probe(&destroyed)](int, short) {}, &scoped) == E::kOk);
    }
    CHECK(destroyed == 2 && ::fcntl(fd.Get(), F_GETFD) >= 0);
}
static void ActiveMoveAndReplacement()
{
    Loop loop;
    auto first = Event();
    auto second = Event();
    Loop::Registration destination, source;
    int calls = 0;
    CHECK(loop.WatchFd(first.Get(), [](int, short) { CHECK(false); }, &destination) == E::kOk);
    CHECK(loop.WatchFd(second.Get(), [&](int fd, short) { Drain(fd); ++calls; loop.Stop(); }, &source) == E::kOk);
    const auto id = source.Id();
    destination = std::move(source);
    CHECK(!source && destination.Id() == id && destination.Fd() == second.Get());
    Ready(first.Get());
    Ready(second.Get());
    CHECK(loop.Run(50) == E::kOk && calls == 1);
    CHECK(loop.WatchFd(first.Get(), [&](int fd, short) { Drain(fd); ++calls; loop.Stop(); }, &destination) == E::kOk);
    CHECK(destination.Id() != id && destination.Fd() == first.Get());
    Ready(second.Get());
    CHECK(loop.Run(50) == E::kOk && calls == 2);
    CHECK(::fcntl(second.Get(), F_GETFD) >= 0);
}

static void DelayedStaleHandle()
{
    Loop loop;
    auto old_fd      = Event();
    const int number = old_fd.Get();
    Loop::Registration old_watch, new_watch;
    int calls = 0;
    CHECK(loop.WatchFd(
              number, [](int, short) { CHECK(false); }, &old_watch) == E::kOk);
    const auto old_id = old_watch.Id();
    loop.RemoveFd(number);
    CHECK(!old_watch);
    old_fd.Reset();
    auto new_fd = Event();
    CHECK(new_fd.Get() == number);
    CHECK(loop.WatchFd(
              number,
              [&](int fd, short) {
                  Drain(fd);
                  ++calls;
                  loop.Stop();
              },
              &new_watch) == E::kOk);
    CHECK(new_watch.Id() != old_id);
    old_watch.Reset();
    CHECK(new_watch && ::fcntl(number, F_GETFD) >= 0);
    Ready(number);
    CHECK(loop.Run(50) == E::kOk && calls == 1);
}
static void SnapshotAndMixedOrder()
{
    Loop loop;
    auto first  = Event();
    auto second = Event();
    auto third  = Event();
    Loop::Registration a, b;
    int order = 0, late_calls = 0;
    CHECK(loop.WatchFd(
              first.Get(),
              [&](int fd, short) {
                  CHECK(++order == 1);
                  Drain(fd);
                  b.Reset();
              },
              &a) == E::kOk);
    loop.AddFd(third.Get(), [&](int fd, short) {
        CHECK(++order == 2);
        Drain(fd);
        loop.Stop();
    });
    CHECK(loop.WatchFd(
              second.Get(), [&](int, short) { ++late_calls; }, &b) == E::kOk);
    Ready(first.Get());
    Ready(second.Get());
    Ready(third.Get());
    CHECK(loop.Run(50) == E::kOk && order == 2 && late_calls == 0);
    loop.RemoveFd(third.Get());
    a.Reset();
    // Add then cancel within a dispatch must not leave a POLLNVAL listener.
    Loop::Registration transient;
    CHECK(loop.WatchFd(
              first.Get(),
              [&](int fd, short) {
                  Drain(fd);
                  CHECK(loop.WatchFd(
                            second.Get(), [&](int, short) { ++late_calls; }, &transient) == E::kOk);
                  transient.Reset();
                  loop.Stop();
              },
              &a) == E::kOk);
    Ready(first.Get());
    CHECK(loop.Run(50) == E::kOk && !transient && late_calls == 0);
}
static void ReplaceDuringCallback()
{
    Loop loop;
    auto fd = Event();
    Loop::Registration watch;
    int calls = 0;
    CHECK(loop.WatchFd(
              fd.Get(),
              [&](int f, short) {
                  Drain(f);
                  ++calls;
                  const auto old = watch.Id();
                  watch.Reset();
                  CHECK(loop.WatchFd(
                            f,
                            [&](int f2, short) {
                                Drain(f2);
                                ++calls;
                                loop.Stop();
                            },
                            &watch) == E::kOk);
                  CHECK(watch.Id() != old);
                  Ready(f);
              },
              &watch) == E::kOk);
    Ready(fd.Get());
    CHECK(loop.Run(50) == E::kOk && calls == 2);
}
static void Timers()
{
    Loop loop;
    Loop::Registration once, periodic;
    bool fired   = false;
    int captured = -1;
    CHECK(loop.WatchOneShotTimer(
              1,
              [&](int fd, short) {
                  fired    = true;
                  captured = fd;
                  CHECK(!once && once.Fd() == -1);
                  struct stat st
                  {};
                  CHECK(::fstat(fd, &st) == 0);
                  once.Reset();
                  CHECK(::fstat(fd, &st) == 0);
                  loop.Stop();
              },
              &once) == E::kOk);
    CHECK(loop.Run(50) == E::kOk && fired);
    CHECK(::fcntl(captured, F_GETFD) == -1 && errno == EBADF);
    int ticks = 0;
    CHECK(loop.WatchTimer(
              1,
              [&](int fd, short) {
                  CHECK(Loop::DrainTimerfd(fd) >= 1);
                  ++ticks;
                  if (ticks == 2)
                  {
                      periodic.Reset();
                      CHECK(::fcntl(fd, F_GETFD) >= 0);
                      loop.Stop();
                  }
              },
              &periodic) == E::kOk);
    const int periodic_fd = periodic.Fd();
    CHECK(loop.Run(50) == E::kOk && ticks == 2);
    CHECK(::fcntl(periodic_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(loop.WatchOneShotTimer(
              100, [](int, short) { CHECK(false); }, &once) == E::kOk);
    const int cancelled_fd = once.Fd();
    once.Reset();
    CHECK(::fcntl(cancelled_fd, F_GETFD) == -1 && errno == EBADF);
    int replacement_calls = 0;
    CHECK(loop.WatchOneShotTimer(
              1,
              [&](int, short) {
                  ++replacement_calls;
                  CHECK(loop.WatchOneShotTimer(
                            1,
                            [&](int, short) {
                                ++replacement_calls;
                                loop.Stop();
                            },
                            &once) == E::kOk);
              },
              &once) == E::kOk);
    CHECK(loop.Run(50) == E::kOk && replacement_calls == 2 && !once);
    // Expired timer handle cannot close a new watched fd with the same number.
    Loop::Registration expired;
    CHECK(loop.WatchOneShotTimer(
              1,
              [&](int fd, short) {
                  captured = fd;
                  loop.Stop();
              },
              &expired) == E::kOk);
    const auto expired_id = expired.Id();
    CHECK(loop.Run(50) == E::kOk && !expired && expired.Id() == expired_id);
    auto replacement = Event();
    CHECK(replacement.Get() == captured);
    Loop::Registration replacement_watch;
    CHECK(loop.WatchFd(
              replacement.Get(),
              [&](int fd, short) {
                  Drain(fd);
                  loop.Stop();
              },
              &replacement_watch) == E::kOk);
    expired.Reset();
    CHECK(replacement_watch && ::fcntl(replacement.Get(), F_GETFD) >= 0);
    Ready(replacement.Get());
    CHECK(loop.Run(50) == E::kOk);
}
static void Busy()
{
    Loop loop;
    Loop::Registration old, current, removed;
    CHECK(loop.WatchBusyFd(
              10000, [](int, short) { return E::kOk; }, &old) == E::kInvalidArgument);
    CHECK(loop.SetBusyPolling(true) == E::kOk);
    CHECK(loop.WatchBusyFd(
              10000,
              [](int, short) {
                  CHECK(false);
                  return E::kOk;
              },
              &old) == E::kOk);
    const auto stale = old.Id();
    loop.RemoveFd(10000);
    int calls = 0, skipped = 0;
    CHECK(loop.WatchBusyFd(
              10000,
              [&](int fd, short) {
                  CHECK(fd == -1);
                  ++calls;
                  old.Reset();
                  removed.Reset();
                  loop.Stop();
                  return E::kOk;
              },
              &current) == E::kOk);
    CHECK(loop.WatchBusyFd(
              10001,
              [&](int, short) {
                  ++skipped;
                  return E::kOk;
              },
              &removed) == E::kOk);
    CHECK(current.Id() != stale && loop.Run() == E::kOk && calls == 1 && skipped == 0);
    current.Reset();
    for (auto error : {E::kProtocol, E::kNoMemory})
    {
        CHECK(loop.WatchBusyFd(
                  10002,
                  [&](int, short) {
                      current.Reset();
                      return error;
                  },
                  &current) == E::kOk);
        CHECK(loop.Run() == error);
    }
    int destroyed = 0;
    fail_nothrow  = 1;
    CHECK(loop.WatchBusyFd(
              10003, [p = Probe(&destroyed)](int, short) { return E::kOk; }, &current) == E::kNoMemory);
    CHECK(!current && destroyed == 1);
}
int main()
{
    ::alarm(15);
    const auto before = Fds();
    LifetimeMoveFailure();
    ActiveMoveAndReplacement();
    DelayedStaleHandle();
    SnapshotAndMixedOrder();
    ReplaceDuringCallback();
    Timers();
    Busy();
    CHECK(Fds() == before);
    std::puts(
        "PASS registration lifetime, move-only storage, allocation rollback, fd/timer reuse, mixed ordering, callback cancellation and busy polling");
}
