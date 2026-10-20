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

namespace {

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
            std::abort();                                             \
        }                                                             \
    } while (0)

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
    loop.AddTimer(500, [&](int tfd, short) {
        shm::EventLoop::DrainTimerfd(tfd);
        loop.Stop();
    });

    int efd1 = ::eventfd(0, EFD_NONBLOCK);
    CHECK(efd1 >= 0, "eventfd");

    loop.AddFd(efd1, [&](int fd, short) {
        uint64_t v = 0;
        (void)::read(fd, &v, sizeof(v));

        // 模拟断连：移除并关闭旧 fd
        loop.RemoveFd(fd);
        ::close(fd);

        // 模拟 accept：新 fd 应立即复用刚关闭的 fd 号（内核取最小可用值）
        int efd2 = ::eventfd(0, EFD_NONBLOCK);
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

    loop.Run(100);

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
    bool armed = false;
    int ticks  = 0;
    loop.AddTimer(20, [&](int tfd, short) {
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
    loop.AddOneShotTimer(300, [&](int, short) { loop.Stop(); });
    loop.Run();
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
    timer_fd = loop.AddOneShotTimer(
        60, [&](int, short) { action_fired = true; });

    // t=25ms 的一次性定时器睡过 t=60ms 再点亮 A，
    // 确保 A 与目标定时器（60ms）在同一 poll 批次就绪
    loop.AddOneShotTimer(25, [&](int, short) {
        ::usleep(45 * 1000);
        uint64_t v = 1;
        CHECK(::write(efd, &v, sizeof(v)) == 8, "write efd");
    });
    loop.AddOneShotTimer(300, [&](int, short) { loop.Stop(); });
    loop.Run();
    ::close(efd);

    CHECK(!action_fired, "cancelled timer action still fired");
    std::printf("  PASS: cancelled timer does not fire\n\n");
}

int main()
{
    TestFdReuseOrdering();
    TestSameBatchAddRemove();
    TestCancelledTimerSilenced();

    std::printf("=== TEST PASSED ===\n");
    return 0;
}
