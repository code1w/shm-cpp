/**
 * @file test_ringbuf_boundary.cpp
 * @brief RingBuf 边界条件测试（工业级审查补充）
 *
 * 与 test_ringbuf_wrap.cpp（回绕正确性）互补，本文件专测边界与误用防护：
 *
 * 1. 满环：填满至 Capacity，写 1 字节被拒，两段式 Peek 读全部内容并校验
 * 2. 满环循环：填满 → 读一半 → 写一半（跨环尾补满）→ 读全部校验
 * 3. BatchWriter 长度边界：len == max_write_size 成功 / +1 拒绝 / 0 拒绝
 * 4. Reserve 边界：len == tail 精确贴合成功 / tail+1 拒绝 / 0 拒绝 /
 *    超大拒绝 / 满环拒绝；Reserve 直写内容校验
 * 5. 空操作边界：Peek 空环 -1、TryRead 空环 0、TryRead max_len=0、
 *    ReadExact len=0，均不消费数据
 * 6. BatchWriter 杂项：空 Flush、重复 Flush、空指针拒绝、移动语义
 * 7. 死亡测试：CommitRead 越界提交触发 assert（SIGABRT）
 * 8. 死亡测试：RewindTo 越过 PendingBytes 触发 assert（SIGABRT）
 *
 * 无测试框架，abort() 失败，打印 "=== TEST PASSED ===" 成功。
 */

#include <shm_ipc/ringbuf.hpp>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// 使用小容量环以低成本覆盖边界
constexpr std::size_t kSmallCap = 4096;
using SmallRing = shm::RingBuf<kSmallCap>;

int g_pass_count = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            std::fprintf(stderr, "FAIL: " __VA_ARGS__);      \
            std::fprintf(stderr, " (line %d)\n", __LINE__);  \
            std::abort();                                    \
        }                                                    \
    } while (0)

/// 分配并初始化一块 shm
void *AllocShm()
{
    void *shm = std::aligned_alloc(64, SmallRing::shm_size);
    CHECK(shm != nullptr, "aligned_alloc");
    SmallRing::Init(shm);
    return shm;
}

/// 构造可预测内容的缓冲区：buf[i] = (seed + i) & 0xFF
void FillPattern(char *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; ++i)
        buf[i] = static_cast<char>((seed + i) & 0xFF);
}

/// 校验缓冲区内容
void VerifyPattern(const char *buf, uint32_t len, uint8_t seed,
                   const char *ctx)
{
    for (uint32_t i = 0; i < len; ++i)
    {
        if (static_cast<uint8_t>(buf[i]) != static_cast<uint8_t>((seed + i) & 0xFF))
        {
            std::fprintf(stderr, "FAIL [%s]: byte[%u]\n", ctx, i);
            std::abort();
        }
    }
}

/// 推进 ring 到指定物理偏移（写入 advance 字节再读走）
void AdvanceRing(void *shm, uint32_t advance)
{
    char tmp[512];
    std::memset(tmp, 0, sizeof(tmp));
    uint32_t remaining = advance;
    while (remaining > 0)
    {
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        CHECK(SmallRing::TryWrite(shm, tmp, chunk) == 0, "AdvanceRing write");
        CHECK(SmallRing::ReadExact(shm, tmp, chunk) == 0, "AdvanceRing read");
        remaining -= chunk;
    }
}

// =========================================================================
// 测试 1：满环 —— 填满至 Capacity，拒绝溢出写，两段式 Peek 校验全部内容
// =========================================================================

void TestFullRing()
{
    std::printf("--- Test 1: full ring (used == Capacity) ---\n");

    void *shm = AllocShm();

    // 让 phys_r 非零，使满环 Peek 走两段路径（r = w = 100）
    AdvanceRing(shm, 100);

    // 填满：2000 + 2000 + 96 = 4096 = Capacity
    std::vector<char> w1(2000), w2(2000), w3(96);
    FillPattern(w1.data(), 2000, 0x01);
    FillPattern(w2.data(), 2000, 0x02);
    FillPattern(w3.data(), 96, 0x03);
    CHECK(SmallRing::TryWrite(shm, w1.data(), 2000) == 0, "write w1");
    CHECK(SmallRing::TryWrite(shm, w2.data(), 2000) == 0, "write w2");
    CHECK(SmallRing::TryWrite(shm, w3.data(), 96) == 0, "write w3");

    // 满环状态：FreeSpace == 0，Available == Capacity
    CHECK(SmallRing::FreeSpace(shm) == 0, "FreeSpace on full ring");
    CHECK(SmallRing::Available(shm) == kSmallCap, "Available on full ring");

    // 满环再写 1 字节必须被拒绝
    CHECK(SmallRing::TryWrite(shm, "x", 1) == -1, "write to full ring");

    // 满环 Peek：phys_r=100 → seg1=3996（尾部），seg2=100（头部）
    const void *seg1 = nullptr, *seg2 = nullptr;
    uint32_t seg1_len = 0, seg2_len = 0;
    CHECK(SmallRing::Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len) == 0,
          "Peek full ring");
    CHECK(seg1_len == kSmallCap - 100, "seg1_len=%u", seg1_len);
    CHECK(seg2_len == 100, "seg2_len=%u", seg2_len);

    // 拼接期望流：w1 + w2 + w3
    std::vector<char> expected;
    expected.insert(expected.end(), w1.begin(), w1.end());
    expected.insert(expected.end(), w2.begin(), w2.end());
    expected.insert(expected.end(), w3.begin(), w3.end());
    CHECK(expected.size() == kSmallCap, "expected size");
    CHECK(std::memcmp(seg1, expected.data(), seg1_len) == 0, "seg1 content");
    CHECK(std::memcmp(seg2, expected.data() + seg1_len, seg2_len) == 0,
          "seg2 content");

    // 全部读走，环恢复为空
    SmallRing::CommitRead(shm, kSmallCap);
    CHECK(SmallRing::Available(shm) == 0, "empty after full drain");
    CHECK(SmallRing::FreeSpace(shm) == kSmallCap, "FreeSpace after drain");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: full ring write reject + two-segment peek verified\n");
}

// =========================================================================
// 测试 2：满环循环 —— 填满 → 读一半 → 写一半（跨环尾）→ 读全部校验
// =========================================================================

void TestFullRingCycle()
{
    std::printf("--- Test 2: full ring cycle (fill/drain/refill) ---\n");

    void *shm = AllocShm();
    AdvanceRing(shm, 700);  // 非零物理偏移起步

    std::vector<char> wa(2048), wb(2048), wc(1500);
    FillPattern(wa.data(), 2048, 0xA1);
    FillPattern(wb.data(), 2048, 0xB1);
    FillPattern(wc.data(), 1500, 0xC1);

    // 填满
    CHECK(SmallRing::TryWrite(shm, wa.data(), 2048) == 0, "write A");
    CHECK(SmallRing::TryWrite(shm, wb.data(), 2048) == 0, "write B");
    CHECK(SmallRing::FreeSpace(shm) == 0, "full");

    // 读走 1500（A 的前 1500 字节）
    std::vector<char> rbuf(4096);
    CHECK(SmallRing::TryRead(shm, rbuf.data(), 1500) == 1500, "drain 1500");
    CHECK(std::memcmp(rbuf.data(), wa.data(), 1500) == 0, "drained content");

    // 补回 1500（写入跨环尾），再次填满
    CHECK(SmallRing::TryWrite(shm, wc.data(), 1500) == 0, "refill C");
    CHECK(SmallRing::FreeSpace(shm) == 0, "full again");

    // 读全部 4096：A[1500..2048) + B[0..2048) + C[0..1500)
    CHECK(SmallRing::TryRead(shm, rbuf.data(), 4096) == 4096, "drain all");
    CHECK(std::memcmp(rbuf.data(), wa.data() + 1500, 548) == 0, "A tail");
    CHECK(std::memcmp(rbuf.data() + 548, wb.data(), 2048) == 0, "B body");
    CHECK(std::memcmp(rbuf.data() + 548 + 2048, wc.data(), 1500) == 0,
          "C body");
    CHECK(SmallRing::Available(shm) == 0, "empty at end");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: fill/drain/refill cycle keeps content intact\n");
}

// =========================================================================
// 测试 3：BatchWriter 长度边界
// =========================================================================

void TestBatchWriterLenBounds()
{
    std::printf("--- Test 3: BatchWriter length bounds ---\n");

    void *shm = AllocShm();
    std::vector<char> buf(SmallRing::max_write_size + 1, 'B');

    {
        SmallRing::BatchWriter batch(shm);

        // len == 0 拒绝
        CHECK(batch.TryWrite(buf.data(), 0) == -1, "len=0");
        // len == max_write_size + 1 拒绝
        CHECK(batch.TryWrite(buf.data(), SmallRing::max_write_size + 1) == -1,
              "len=max+1");
        // data == nullptr 拒绝
        CHECK(batch.TryWrite(nullptr, 10) == -1, "null data");
        CHECK(batch.Count() == 0 && batch.PendingBytes() == 0,
              "rejected writes must not count");

        // len == max_write_size 恰好成功
        CHECK(batch.TryWrite(buf.data(), SmallRing::max_write_size) == 0,
              "len=max");
        // 再写一个 max_write_size：恰好填满
        CHECK(batch.TryWrite(buf.data(), SmallRing::max_write_size) == 0,
              "len=max second (fills ring)");
        CHECK(batch.FreeBytes() == 0, "full after 2 x max");
        // 满环再写 1 字节拒绝
        CHECK(batch.TryWrite(buf.data(), 1) == -1, "full ring write");
        CHECK(batch.Flush() == 2, "flush count");
    }

    CHECK(SmallRing::Available(shm) == kSmallCap, "full ring published");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: len bounds 0 / max / max+1 / full-ring\n");
}

// =========================================================================
// 测试 4：Reserve 边界
// =========================================================================

void TestReserveBoundaries()
{
    std::printf("--- Test 4: Reserve boundaries ---\n");

    void *shm = AllocShm();
    constexpr uint32_t kMax = SmallRing::max_write_size;  // 2048

    // phys=0：基本拒绝项 + 正常 Reserve/CommitReserve 直写校验
    {
        SmallRing::BatchWriter b(shm);
        CHECK(b.Reserve(0) == nullptr, "Reserve(0)");
        CHECK(b.Reserve(kMax + 1) == nullptr, "Reserve(max+1)");

        char *p = b.Reserve(kMax);
        CHECK(p != nullptr, "Reserve(max)");
        FillPattern(p, kMax, 0x55);  // 直写环内
        b.CommitReserve(kMax);
        CHECK(b.Flush() == 1, "flush");
    }
    {
        std::vector<char> rbuf(kMax);
        CHECK(SmallRing::ReadExact(shm, rbuf.data(), kMax) == 0, "read back");
        VerifyPattern(rbuf.data(), kMax, 0x55, "Reserve direct write");
    }
    // 当前 w = r = 2048，phys = 2048

    // 推进到 tail=100（phys=3996）：advance = 3996-2048 = 1948
    AdvanceRing(shm, 1948);
    {
        SmallRing::BatchWriter b(shm);
        // len == tail：精确贴合，允许（不跨环尾）
        char *p = b.Reserve(100);
        CHECK(p != nullptr, "Reserve(tail) exact fit");
        FillPattern(p, 100, 0x66);
        b.CommitReserve(100);
        // 现在 phys=0：继续 Reserve 50
        p = b.Reserve(50);
        CHECK(p != nullptr, "Reserve(50) at phys=0");
        FillPattern(p, 50, 0x77);
        b.CommitReserve(50);
        CHECK(b.Flush() == 2, "flush 2");
    }
    {
        std::vector<char> rbuf(150);
        CHECK(SmallRing::ReadExact(shm, rbuf.data(), 150) == 0,
              "read wrapped reserves");
        VerifyPattern(rbuf.data(), 100, 0x66, "reserve seg1");
        VerifyPattern(rbuf.data() + 100, 50, 0x77, "reserve seg2");
    }
    // 当前 w = r = 4146，phys = 50

    // 推进到 tail=50（phys=4046）：advance = 4046-50 = 3996
    AdvanceRing(shm, 3996);
    {
        SmallRing::BatchWriter b(shm);
        CHECK(b.Reserve(51) == nullptr, "Reserve(tail+1) must wrap-reject");
        CHECK(b.Reserve(50) != nullptr, "Reserve(tail) exact fit allowed");
        // 不 CommitReserve，析构 Flush 不应发布任何字节
    }

    // 满环时 Reserve 拒绝；Cancel 后环无变化
    {
        SmallRing::BatchWriter b(shm);
        std::vector<char> fill(kMax, 'F');
        CHECK(b.TryWrite(fill.data(), kMax) == 0, "fill 1");
        CHECK(b.TryWrite(fill.data(), kMax) == 0, "fill 2");
        CHECK(b.Reserve(1) == nullptr, "Reserve on full ring");
        b.Cancel();  // 丢弃全部未发布写入
    }
    CHECK(SmallRing::Available(shm) == 0, "Cancel leaves ring untouched");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: Reserve exact-fit / wrap / oversize / full-ring\n");
}

// =========================================================================
// 测试 5：空操作边界 —— 均不得消费数据
// =========================================================================

void TestEmptyOps()
{
    std::printf("--- Test 5: empty/zero-length ops ---\n");

    void *shm = AllocShm();
    char buf[128];

    // 空环
    const void *seg1 = nullptr, *seg2 = nullptr;
    uint32_t seg1_len = 0, seg2_len = 0;
    CHECK(SmallRing::Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len) == -1,
          "Peek empty ring");
    CHECK(SmallRing::TryRead(shm, buf, sizeof(buf)) == 0, "TryRead empty");
    CHECK(SmallRing::ReadExact(shm, buf, 10) == -1, "ReadExact empty");

    // 写入 50 字节
    FillPattern(buf, 50, 0x99);
    CHECK(SmallRing::TryWrite(shm, buf, 50) == 0, "write 50");

    // max_len=0：返回 0 且不消费
    CHECK(SmallRing::TryRead(shm, buf, 0) == 0, "TryRead max_len=0");
    CHECK(SmallRing::Available(shm) == 50, "max_len=0 must not consume");

    // ReadExact len=0：成功且不消费
    CHECK(SmallRing::ReadExact(shm, buf, 0) == 0, "ReadExact len=0");
    CHECK(SmallRing::Available(shm) == 50, "len=0 must not consume");

    // CommitRead(0)：不推进
    SmallRing::CommitRead(shm, 0);
    CHECK(SmallRing::Available(shm) == 50, "CommitRead(0) must not consume");

    // 数据仍完整可读
    char out[50];
    CHECK(SmallRing::ReadExact(shm, out, 50) == 0, "read after zero ops");
    VerifyPattern(out, 50, 0x99, "zero ops preserve data");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: zero-length ops never consume\n");
}

// =========================================================================
// 测试 6：BatchWriter 杂项 —— 空 Flush、重复 Flush、空指针、移动语义
// =========================================================================

void TestBatchWriterMisc()
{
    std::printf("--- Test 6: BatchWriter misc ---\n");

    void *shm = AllocShm();

    {
        SmallRing::BatchWriter b(shm);
        // 空 Flush：返回 0 且不发布
        CHECK(b.Flush() == 0, "empty flush");
        CHECK(b.Flush() == 0, "double empty flush");

        // 全部拒绝项不计数
        CHECK(b.TryWrite(nullptr, 10) == -1, "null data");
        CHECK(b.TryWrite("x", 0) == -1, "len=0");
        CHECK(b.Flush() == 0, "flush after rejects");

        // 移动语义：移动后 Flush 生效，moved-from 安全
        CHECK(b.TryWrite("abc", 3) == 0, "write abc");
        SmallRing::BatchWriter b2(std::move(b));
        CHECK(b2.Flush() == 1, "moved flush publishes");
        CHECK(b.Flush() == 0, "moved-from flush is no-op");
    }  // 两个析构均安全

    CHECK(SmallRing::Available(shm) == 3, "moved batch published 3 bytes");
    char out[3];
    CHECK(SmallRing::ReadExact(shm, out, 3) == 0, "read moved batch data");
    CHECK(std::memcmp(out, "abc", 3) == 0, "moved batch content");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: empty flush / move semantics / moved-from safety\n");
}

// =========================================================================
// 测试 7（死亡测试）：CommitRead 越界提交必须触发 assert
// =========================================================================

void TestCommitReadOverrunAssert()
{
    std::printf("--- Test 7: CommitRead overrun assert (death test) ---\n");

    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); std::abort(); }
    if (pid == 0)
    {
        void *shm = AllocShm();
        char buf[10] = {};
        (void)SmallRing::TryWrite(shm, buf, 10);
        SmallRing::CommitRead(shm, 11);  // 越界提交 → assert → SIGABRT
        std::_Exit(0);                   // 不应到达
    }

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid, "waitpid");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "expected SIGABRT from overrun CommitRead, status=0x%x", status);

    ++g_pass_count;
    std::printf("  PASS: overrun CommitRead aborts in debug build\n");
}

// =========================================================================
// 测试 8（死亡测试）：RewindTo 越过 PendingBytes 必须触发 assert
// =========================================================================

void TestRewindOverrunAssert()
{
    std::printf("--- Test 8: RewindTo overrun assert (death test) ---\n");

    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); std::abort(); }
    if (pid == 0)
    {
        void *shm = AllocShm();
        SmallRing::BatchWriter b(shm);
        char buf[10] = {};
        (void)b.TryWrite(buf, 10);
        // pending 最多为 10，回滚到 11 是调用方错误 → assert → SIGABRT
        b.RewindTo(11, 1);
        std::_Exit(0);  // 不应到达
    }

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid, "waitpid");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "expected SIGABRT from overrun RewindTo, status=0x%x", status);

    ++g_pass_count;
    std::printf("  PASS: overrun RewindTo aborts in debug build\n");
}

}  // anonymous namespace

int main()
{
    std::printf("=== RingBuf boundary test suite (Capacity=%zu) ===\n\n",
                kSmallCap);

    TestFullRing();
    TestFullRingCycle();
    TestBatchWriterLenBounds();
    TestReserveBoundaries();
    TestEmptyOps();
    TestBatchWriterMisc();
    TestCommitReadOverrunAssert();
    TestRewindOverrunAssert();

    std::printf("\n=== TEST PASSED === (%d sub-tests)\n", g_pass_count);
    return 0;
}
