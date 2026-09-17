/**
 * @file test_eventloop.cpp
 * @brief EventLoop 延迟增删的顺序回归测试
 *
 * 回归场景：回调分发期间旧连接断连（RemoveFd + close），新连接在同一轮
 * accept 并复用了同一 fd 号（AddFd）。若 ApplyPendingAdditions 先于
 * ApplyPendingRemovals 执行，removals 按 fd 号匹配会把新注册的条目误删，
 * 导致新连接永远收不到事件。修复后顺序为先删后加。
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

int main()
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

    if (!reused_cb_fired)
    {
        std::fprintf(stderr,
                     "FAIL: reused-fd callback never fired "
                     "(new entry was wrongly removed)\n");
        return 1;
    }

    std::printf("  PASS: fd reused during dispatch, new entry survives\n");
    std::printf("\n=== TEST PASSED ===\n");
    return 0;
}
