/**
 * @file test_eventloop.cpp
 * @brief EventLoop 增删语义回归测试
 *
 * 测试 1（fd 复用顺序）：回调分发期间旧连接断连（RemoveFd + close），
 * 新连接在同一轮 accept 并复用了同一 fd 号（AddFd）。移除按"条目"而非
 * 裸 fd 号生效，新注册的条目不被误删，新连接正常收到事件。
 *
 * 测试 2（同轮注册+注销）：同一分发批次内 AddFd 后立即 RemoveFd，
 * 条目必须真正移除——否则连接对象析构关闭 fd 后，残留条目会让 poll
 * 永远立即返回 POLLNVAL，事件循环 100% CPU 空转（回归）。
 *
 * 测试 3（取消的定时器不补触发）：分发批次内被 RemoveTimer 取消的
 * 一次性定时器，不得再用 poll 时捕获的旧 revents 补触发回调
 * （对应场景：验证应答与验证超时同批就绪，验证成功后连接仍被
 * "超时"断连。回归）。
 */

#include <shm_ipc/event_loop.hpp>

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <string>
#include <stdexcept>
#include <thread>
#include <atomic>

namespace {

#define CHECK(cond, msg)                                                 \
    do                                                                   \
    {                                                                    \
        if (!(cond))                                                     \
        {                                                                \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            std::abort();                                                \
        }                                                                \
    } while (0)

void AddTimerOrAbort(shm::EventLoop& loop, int ms, shm::EventLoop::Callback cb)
{
    int tfd = 0;
    if (loop.AddTimer(ms, std::move(cb), &tfd) != shm::ErrorCode::kOk)
    {
        std::fprintf(stderr, "FAIL: AddTimer\n");
        std::abort();
    }
}

void AddOneShotOrAbort(shm::EventLoop& loop, int ms, shm::EventLoop::Callback cb, int* out = nullptr)
{
    int tfd = 0;
    if (loop.AddOneShotTimer(ms, std::move(cb), &tfd) != shm::ErrorCode::kOk)
    {
        std::fprintf(stderr, "FAIL: AddOneShotTimer\n");
        std::abort();
    }
    if (out)
    {
        *out = tfd;
    }
}

}  // anonymous namespace

// =========================================================================
// 测试 1：分发期间 remove 旧 fd + add 复用同号的新 fd
// =========================================================================
static void TestFdReuseOrdering()
{
    std::printf("=== EventLoop fd-reuse ordering test ===\n");

    shm::EventLoop loop;
    bool reused_cb_fired = false;

    // 看门狗：500ms 后强制停止，修复失效（回调永不触发）时避免测试挂死
    AddTimerOrAbort(loop, 500, [&](int tfd, short) {
        shm::EventLoop::DrainTimerfd(tfd);
        loop.Stop();
    });

    const int efd1 = ::eventfd(0, EFD_NONBLOCK);
    CHECK(efd1 >= 0, "eventfd");

    loop.AddFd(efd1, [&](int fd, short) {
        uint64_t v = 0;
        (void)::read(fd, &v, sizeof(v));

        // 模拟断连：移除并关闭旧 fd
        loop.RemoveFd(fd);
        ::close(fd);

        // 模拟 accept：新 fd 应立即复用刚关闭的 fd 号（内核取最小可用值）
        const int efd2 = ::eventfd(0, EFD_NONBLOCK);
        CHECK(efd2 == fd, "fd number not reused");

        loop.AddFd(efd2, [&](int fd2, short) {
            uint64_t v2 = 0;
            (void)::read(fd2, &v2, sizeof(v2));
            reused_cb_fired = true;
            loop.Stop();
        });

        // 让新 fd 立即可读，下一轮 poll 即应触发其回调
        uint64_t one = 1;
        CHECK(::write(efd2, &one, sizeof(one)) == 8, "write efd2");
    });

    uint64_t one = 1;
    CHECK(::write(efd1, &one, sizeof(one)) == 8, "write efd1");

    (void)loop.Run(100);

    CHECK(reused_cb_fired, "reused-fd callback never fired");
    std::printf("  PASS: fd reused during dispatch, new entry survives\n\n");
}

// =========================================================================
// 测试 2（回归）：同一分发批次内 AddFd + RemoveFd，条目必须真正移除
// =========================================================================
static void TestSameBatchAddRemove()
{
    std::printf("=== EventLoop same-batch add+remove test ===\n");

    shm::EventLoop loop;
    int pfd[2];
    CHECK(::pipe(pfd) == 0, "pipe");

    int stale_dispatch = 0;
    bool armed         = false;
    int ticks          = 0;
    AddTimerOrAbort(loop, 20, [&](int tfd, short) {
        shm::EventLoop::DrainTimerfd(tfd);
        if (++ticks == 1)
        {
            // 模拟 OnAccept：dispatch 期间注册新连接 fd，
            // 随后同轮内连接被关闭注销（如 connection 回调里 CloseConn）
            loop.AddFd(pfd[0], [&](int, short) { ++stale_dispatch; });
            loop.RemoveFd(pfd[0]);
            armed = true;
        }
        else if (ticks == 2)
        {
            // 模拟连接对象析构关闭 fd：若条目泄漏，
            // poll 将 POLLNVAL 忙轮询，stale_dispatch 暴涨
            ::close(pfd[0]);
        }
    });
    AddOneShotOrAbort(loop, 300, [&](int, short) { loop.Stop(); });
    (void)loop.Run();
    ::close(pfd[1]);

    CHECK(armed, "timer never fired");
    CHECK(stale_dispatch == 0, "removed fd still dispatched (entry leaked)");
    std::printf("  PASS: same-batch add+remove entry actually removed\n\n");
}

// =========================================================================
// 测试 3（回归）：分发批次内被取消的一次性定时器不得补触发
// =========================================================================
static void TestCancelledTimerSilenced()
{
    std::printf("=== EventLoop cancelled one-shot timer test ===\n");

    shm::EventLoop loop;
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    CHECK(efd >= 0, "eventfd");

    bool action_fired = false;
    int timer_fd      = -1;

    // A 先注册：与目标定时器同一 poll 批次就绪时，A 的回调先执行并取消它
    loop.AddFd(efd, [&](int fd, short) {
        uint64_t v = 0;
        (void)::read(fd, &v, sizeof(v));
        loop.RemoveTimer(timer_fd);
    });
    AddOneShotOrAbort(
        loop, 60, [&](int, short) { action_fired = true; }, &timer_fd);

    // t=25ms 的一次性定时器睡过 t=60ms 再点亮 A，
    // 确保 A 与目标定时器（60ms）在同一 poll 批次就绪
    AddOneShotOrAbort(loop, 25, [&](int, short) {
        ::usleep(45 * 1000);
        uint64_t v = 1;
        CHECK(::write(efd, &v, sizeof(v)) == 8, "write efd");
    });
    AddOneShotOrAbort(loop, 300, [&](int, short) { loop.Stop(); });
    (void)loop.Run();
    ::close(efd);

    CHECK(!action_fired, "cancelled timer action still fired");
    std::printf("  PASS: cancelled timer does not fire\n\n");
}


// 回归：Stop 前后均不得重入；回调异常/错误码退出后运行所有权必须释放。
static void TestRunReentryAndRecovery()
{
    for (bool stop_before : {false, true})
    {
        for (int failure : {0, 1, 2})
        {
            shm::EventLoop loop;
            shm::UniqueFd fd;
            CHECK(shm::CreateEventfd(&fd) == shm::ErrorCode::kOk, "eventfd");
            const uint64_t one = 1;
            CHECK(::write(fd.Get(), &one, sizeof(one)) == 8, "write eventfd");
            bool survived = false;
            loop.AddFd(fd.Get(), [&, payload = std::string(1000, 'x')](int f, short) {
                uint64_t v;
                CHECK(::read(f, &v, sizeof(v)) == 8, "read eventfd");
                loop.RemoveFd(f);
                if (stop_before) loop.Stop();
                int error = 0;
                CHECK(loop.Run(0, &error) == shm::ErrorCode::kInvalidArgument && error == EBUSY,
                      "nested Run must reject without dispatching or compacting");
                CHECK(payload[0] == 'x' && payload.size() == 1000, "active callback capture was destroyed");
                survived = true;
                if (failure == 1) throw std::runtime_error("callback");
                if (failure == 2) throw std::bad_alloc{};
                loop.Stop();
            });
            bool caught = false;
            shm::ErrorCode result = shm::ErrorCode::kOk;
            try { result = loop.Run(-1); }
            catch (const std::runtime_error&) { caught = true; }
            CHECK(survived && caught == (failure == 1), "callback exception propagation");
            CHECK(result == (failure == 2 ? shm::ErrorCode::kNoMemory : shm::ErrorCode::kOk), "Run result");
            int timer; bool restarted = false;
            CHECK(loop.AddOneShotTimer(1, [&](int, short) { restarted = true; loop.Stop(); }, &timer) == shm::ErrorCode::kOk,
                  "restart timer");
            CHECK(loop.Run(-1) == shm::ErrorCode::kOk && restarted, "Run guard leaked after exit");
        }
    }
    // Stop 后的 post-dispatch 钩子也仍处于外层 Run 生命周期中。
    shm::EventLoop loop;
    shm::EventLoop::HookId hook;
    bool checked = false;
    CHECK(loop.AddPostDispatchHook([&] {
        int error = 0;
        CHECK(loop.Run(0, &error) == shm::ErrorCode::kInvalidArgument && error == EBUSY, "hook reentry");
        checked = true;
    }, &hook) == shm::ErrorCode::kOk, "post hook");
    int timer;
    CHECK(loop.AddOneShotTimer(1, [&](int, short) { loop.Stop(); }, &timer) == shm::ErrorCode::kOk, "stop timer");
    CHECK(loop.Run(-1) == shm::ErrorCode::kOk && checked, "post hook lifecycle");
    loop.RemovePostDispatchHook(hook);
    std::printf("  PASS: Run rejects reentry and recovers after all exit paths\n\n");
}

static void TestConcurrentRunRejected()
{
    shm::EventLoop loop;
    std::atomic<bool> entered{false}, contender_done{false};
    int timer;
    CHECK(loop.AddOneShotTimer(1, [&](int, short) {
        entered.store(true, std::memory_order_release);
        while (!contender_done.load(std::memory_order_acquire)) std::this_thread::yield();
        loop.Stop();
    }, &timer) == shm::ErrorCode::kOk, "concurrent stop timer");
    std::thread contender([&] {
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        int error = 0;
        CHECK(loop.Run(0, &error) == shm::ErrorCode::kInvalidArgument && error == EBUSY, "concurrent Run");
        contender_done.store(true, std::memory_order_release);
    });
    CHECK(loop.Run(-1) == shm::ErrorCode::kOk, "outer Run");
    contender.join();
    std::printf("  PASS: concurrent Run rejected before accessing loop state\n\n");
}

static void TestBusyPolling()
{
    using E = shm::ErrorCode;
    shm::EventLoop loop;
    CHECK(loop.SetBusyPolling(true, 0) == E::kInvalidArgument, "zero control interval");
    CHECK(!loop.BusyPolling(), "invalid configuration leaves mode unchanged");
    CHECK(loop.SetBusyPolling(true, 4) == E::kOk, "configure busy mode");
    shm::EventLoop default_loop;
    CHECK(default_loop.AddBusyPollFd(10000, [](int, short) { return E::kOk; }) == E::kInvalidArgument,
          "busy registration requires opt-in");
    CHECK(loop.AddBusyPollFd(10000, static_cast<shm::EventLoop::PollCallback>(nullptr)) == E::kInvalidArgument,
          "null callback rejected");
    CHECK(loop.AddBusyPollFd(-1, [](int, short) { return E::kOk; }) == E::kInvalidArgument,
          "negative identity rejected");
    int first = 0, replacement = 0, removed = 0;
    CHECK(loop.AddBusyPollFd(10001, [&](int fd, short events) {
        CHECK(fd == -1 && events == 0, "poll callback must not read fd");
        CHECK(loop.SetBusyPolling(false) == E::kInvalidArgument, "no mode change during Run");
        ++first;
        loop.RemoveFd(10001);
        loop.RemoveFd(10002);
        return loop.AddBusyPollFd(10001, [&](int, short) {
            ++replacement;
            loop.Stop();
            return E::kOk;
        });
    }) == E::kOk, "register first callback");
    CHECK(loop.AddBusyPollFd(10002, [&](int, short) { ++removed; return E::kOk; }) == E::kOk, "register removed callback");
    CHECK(loop.Run(-1) == E::kOk, "busy Run");
    CHECK(first == 1 && replacement == 1 && removed == 0, "busy snapshot and tombstones");
    loop.RemoveFd(10001);
    int ticks = 0;
    CHECK(loop.AddBusyPollFd(10003, [&](int, short) { ++ticks; return E::kOk; }) == E::kOk, "register timer observer");
    int timer;
    CHECK(loop.AddOneShotTimer(2, [&](int, short) { loop.Stop(); }, &timer) == E::kOk, "busy timer");
    CHECK(loop.Run(-1) == E::kOk && ticks > 0, "busy control fds and restart");
    loop.RemoveFd(10003);
    for (E error : {E::kProtocol, E::kNoMemory})
    {
        CHECK(loop.AddBusyPollFd(10004, [&](int, short) {
            loop.RemoveFd(10004);
            return error;
        }) == E::kOk, "register failing callback");
        CHECK(loop.Run() == error, "busy callback error propagated");
        CHECK(loop.AddBusyPollFd(10005, [&](int, short) {
            loop.RemoveFd(10005);
            loop.Stop();
            return E::kOk;
        }) == E::kOk, "register recovery callback");
        CHECK(loop.Run() == E::kOk, "callback error releases Run ownership");
    }
    shm::EventLoop threaded;
    CHECK(threaded.SetBusyPolling(true) == E::kOk, "configure threaded loop");
    std::atomic<bool> entered{false};
    CHECK(threaded.AddBusyPollFd(10006, [&](int, short) {
        entered.store(true, std::memory_order_release);
        return E::kOk;
    }) == E::kOk, "register stop observer");
    std::thread stopper([&] {
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        threaded.Stop();
    });
    CHECK(threaded.Run(-1) == E::kOk, "cross-thread stop while polling");
    stopper.join();
    std::printf("  PASS: busy polling lifecycle, timers, removal, error returns\n");
}

int main()
{
    TestFdReuseOrdering();
    TestSameBatchAddRemove();
    TestCancelledTimerSilenced();
    TestRunReentryAndRecovery();
    TestConcurrentRunRejected();
    TestBusyPolling();

    std::printf("=== TEST PASSED ===\n");
    return 0;
}
