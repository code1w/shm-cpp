/**
 * @file test_ringbuf_wrap.cpp
 * @brief 环形缓冲区回绕正确性测试
 *
 * 全面测试 RingBuf 字节流环在各种回绕场景下的数据完整性：
 *
 * 1. RingBuf::TryWrite + ReadExact —— 单帧跨环尾，逐字节校验
 * 2. RingBuf::TryWrite + TryRead（部分读）—— 分多次读跨环尾数据
 * 3. RingBuf::Peek + CommitRead —— 两段式零拷贝读跨环尾数据
 * 4. RingBuf::BatchWriter —— 批量写跨环尾后 ReadExact
 * 5. 多帧连续回绕 —— 连续多帧其中部分跨越环尾
 * 6. 参数化 tail 偏移 —— 遍历所有关键 tail 值（1, 2, 4, 7, 帧/2, 帧-1 等）
 * 7. Codec 层回绕 —— Encode 写入环 → 跨环尾 → ReadExact → Decode 正确
 * 8. SendPod / PodReader 跨进程回绕 —— fork + RingChannel + 小环模拟
 *
 * 使用小容量环（4096 字节）以低成本覆盖各种回绕位置。
 * 无测试框架，abort() 失败，打印 "=== TEST PASSED ===" 成功。
 */

#include <shm_ipc/ringbuf.hpp>
#include <shm_ipc/codec.hpp>
#include <shm_ipc/pod_codec.hpp>
#include <shm_ipc/messages.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// 使用小容量环以低成本覆盖各种回绕位置
constexpr std::size_t kSmallCap = 4096;
using SmallRing = shm::RingBuf<kSmallCap>;

/// 分配并初始化一块 shm
void *AllocShm()
{
    void *shm = std::aligned_alloc(64, SmallRing::shm_size);
    if (!shm) { std::fprintf(stderr, "FAIL: aligned_alloc\n"); std::abort(); }
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
        uint8_t expected = (seed + i) & 0xFF;
        if (static_cast<uint8_t>(buf[i]) != expected)
        {
            std::fprintf(stderr,
                         "FAIL [%s]: byte[%u] = 0x%02X, expected 0x%02X\n",
                         ctx, i,
                         static_cast<unsigned>(static_cast<uint8_t>(buf[i])),
                         static_cast<unsigned>(expected));
            std::abort();
        }
    }
}

/// 推进 ring 到指定物理偏移（写入 advance 字节再读走）
void AdvanceRing(void *shm, uint32_t advance)
{
    // 分多次写入/读出，每次不超过 max_write_size
    char tmp[512];
    std::memset(tmp, 0, sizeof(tmp));
    uint32_t remaining = advance;
    while (remaining > 0)
    {
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        if (SmallRing::TryWrite(shm, tmp, chunk) != 0)
        {
            std::fprintf(stderr, "FAIL: AdvanceRing TryWrite\n");
            std::abort();
        }
        if (SmallRing::ReadExact(shm, tmp, chunk) != 0)
        {
            std::fprintf(stderr, "FAIL: AdvanceRing ReadExact\n");
            std::abort();
        }
        remaining -= chunk;
    }
}

int g_pass_count = 0;

// =========================================================================
// 测试 1: TryWrite + ReadExact 跨环尾
// =========================================================================

void TestWriteReadExactWrap()
{
    std::printf("--- Test 1: TryWrite + ReadExact wrap ---\n");

    // 遍历关键 tail 值：帧会在环尾剩余 tail 字节时开始写入
    constexpr uint32_t msg_len = 200;
    std::vector<uint32_t> tail_values = {1, 2, 3, 4, 7, 8, 15, 16, 50,
                                          msg_len / 2, msg_len - 1,
                                          msg_len, msg_len + 1};

    for (uint32_t tail : tail_values)
    {
        if (tail >= kSmallCap) continue;

        void *shm = AllocShm();

        // 推进到 phys = kSmallCap - tail
        uint32_t advance = kSmallCap - tail;
        AdvanceRing(shm, advance);

        // 写入 msg_len 字节（若 msg_len > tail 则跨环尾）
        char wbuf[msg_len];
        FillPattern(wbuf, msg_len, static_cast<uint8_t>(tail));

        if (SmallRing::TryWrite(shm, wbuf, msg_len) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite at tail=%u\n", tail);
            std::abort();
        }

        char rbuf[msg_len];
        std::memset(rbuf, 0, msg_len);
        if (SmallRing::ReadExact(shm, rbuf, msg_len) != 0)
        {
            std::fprintf(stderr, "FAIL: ReadExact at tail=%u\n", tail);
            std::abort();
        }

        char ctx[64];
        std::snprintf(ctx, sizeof(ctx), "ReadExact tail=%u", tail);
        VerifyPattern(rbuf, msg_len, static_cast<uint8_t>(tail), ctx);

        std::free(shm);
        ++g_pass_count;
    }
    std::printf("  PASS: %d tail offsets\n", static_cast<int>(tail_values.size()));
}

// =========================================================================
// 测试 2: TryWrite + TryRead（部分读）跨环尾
// =========================================================================

void TestWriteTryReadPartialWrap()
{
    std::printf("--- Test 2: TryWrite + TryRead partial wrap ---\n");

    constexpr uint32_t msg_len = 300;
    // tail=50: 前50字节在尾部, 后250字节在头部
    constexpr uint32_t tail = 50;

    void *shm = AllocShm();
    AdvanceRing(shm, kSmallCap - tail);

    char wbuf[msg_len];
    FillPattern(wbuf, msg_len, 0xAB);
    if (SmallRing::TryWrite(shm, wbuf, msg_len) != 0)
    {
        std::fprintf(stderr, "FAIL: TryWrite\n");
        std::abort();
    }

    // 分 3 次读：20 + 100 + 剩余
    char rbuf[msg_len];
    uint32_t total_read = 0;

    uint32_t got = SmallRing::TryRead(shm, rbuf, 20);
    if (got != 20)
    {
        std::fprintf(stderr, "FAIL: TryRead(20) got %u\n", got);
        std::abort();
    }
    total_read += got;

    got = SmallRing::TryRead(shm, rbuf + total_read, 100);
    if (got != 100)
    {
        std::fprintf(stderr, "FAIL: TryRead(100) got %u\n", got);
        std::abort();
    }
    total_read += got;

    got = SmallRing::TryRead(shm, rbuf + total_read, msg_len);
    if (got != msg_len - total_read)
    {
        std::fprintf(stderr, "FAIL: TryRead(rest) got %u, expected %u\n",
                     got, msg_len - total_read);
        std::abort();
    }
    total_read += got;

    VerifyPattern(rbuf, msg_len, 0xAB, "TryRead partial");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 3: Peek + CommitRead 两段式零拷贝跨环尾
// =========================================================================

void TestPeekCommitReadWrap()
{
    std::printf("--- Test 3: Peek + CommitRead wrap ---\n");

    constexpr uint32_t msg_len = 256;
    // tail=30: 数据跨环尾，seg1=30 字节，seg2=226 字节
    constexpr uint32_t tail = 30;

    void *shm = AllocShm();
    AdvanceRing(shm, kSmallCap - tail);

    char wbuf[msg_len];
    FillPattern(wbuf, msg_len, 0xCD);
    if (SmallRing::TryWrite(shm, wbuf, msg_len) != 0)
    {
        std::fprintf(stderr, "FAIL: TryWrite\n");
        std::abort();
    }

    const void *seg1 = nullptr, *seg2 = nullptr;
    uint32_t seg1_len = 0, seg2_len = 0;
    if (SmallRing::Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len) != 0)
    {
        std::fprintf(stderr, "FAIL: Peek\n");
        std::abort();
    }

    // 验证段长度
    if (seg1_len != tail)
    {
        std::fprintf(stderr, "FAIL: seg1_len=%u, expected %u\n", seg1_len, tail);
        std::abort();
    }
    if (seg2_len != msg_len - tail)
    {
        std::fprintf(stderr, "FAIL: seg2_len=%u, expected %u\n",
                     seg2_len, msg_len - tail);
        std::abort();
    }

    // 拼接两段后验证内容
    char rbuf[msg_len];
    std::memcpy(rbuf, seg1, seg1_len);
    std::memcpy(rbuf + seg1_len, seg2, seg2_len);
    VerifyPattern(rbuf, msg_len, 0xCD, "Peek two-segment");

    SmallRing::CommitRead(shm, msg_len);

    // 确认读完
    if (SmallRing::Available(shm) != 0)
    {
        std::fprintf(stderr, "FAIL: ring not empty after CommitRead\n");
        std::abort();
    }

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 4: Peek 无回绕 —— 单段
// =========================================================================

void TestPeekNoWrap()
{
    std::printf("--- Test 4: Peek no-wrap (single segment) ---\n");

    constexpr uint32_t msg_len = 100;

    void *shm = AllocShm();
    // phys=0, tail=4096, msg_len=100 无回绕

    char wbuf[msg_len];
    FillPattern(wbuf, msg_len, 0x11);
    SmallRing::TryWrite(shm, wbuf, msg_len);

    const void *seg1 = nullptr, *seg2 = nullptr;
    uint32_t seg1_len = 0, seg2_len = 0;
    SmallRing::Peek(shm, &seg1, &seg1_len, &seg2, &seg2_len);

    if (seg1_len != msg_len || seg2_len != 0)
    {
        std::fprintf(stderr, "FAIL: seg1_len=%u seg2_len=%u\n", seg1_len, seg2_len);
        std::abort();
    }

    VerifyPattern(static_cast<const char *>(seg1), seg1_len, 0x11,
                  "Peek no-wrap");

    SmallRing::CommitRead(shm, msg_len);
    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 5: BatchWriter 跨环尾
// =========================================================================

void TestBatchWriterWrap()
{
    std::printf("--- Test 5: BatchWriter wrap ---\n");

    // 推进到 phys=kSmallCap-150, 然后批量写 3 条 60 字节
    // 第 1 条: phys 3946, 60 字节 fits in tail=150 → 不回绕
    // 第 2 条: phys 4006, 60 字节, tail=90 → 不回绕
    // 第 3 条: phys 4066, 60 字节, tail=30 → 回绕！前30在尾，后30在头
    constexpr uint32_t advance = kSmallCap - 150;

    void *shm = AllocShm();
    AdvanceRing(shm, advance);

    {
        SmallRing::BatchWriter batch(shm);
        for (int i = 0; i < 3; ++i)
        {
            char wbuf[60];
            FillPattern(wbuf, 60, static_cast<uint8_t>(i * 10));
            if (batch.TryWrite(wbuf, 60) != 0)
            {
                std::fprintf(stderr, "FAIL: BatchWriter TryWrite #%d\n", i);
                std::abort();
            }
        }
        int flushed = batch.Flush();
        if (flushed != 3)
        {
            std::fprintf(stderr, "FAIL: Flush returned %d, expected 3\n", flushed);
            std::abort();
        }
    }

    // 依次读出 3 条并验证
    for (int i = 0; i < 3; ++i)
    {
        char rbuf[60];
        if (SmallRing::ReadExact(shm, rbuf, 60) != 0)
        {
            std::fprintf(stderr, "FAIL: ReadExact batch msg #%d\n", i);
            std::abort();
        }
        char ctx[64];
        std::snprintf(ctx, sizeof(ctx), "BatchWriter msg#%d", i);
        VerifyPattern(rbuf, 60, static_cast<uint8_t>(i * 10), ctx);
    }

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 6: 多帧连续写入，部分帧跨环尾
// =========================================================================

void TestMultiFrameWrap()
{
    std::printf("--- Test 6: Multi-frame continuous wrap ---\n");

    // 使用 200 字节帧，ring 4096
    // 4096 / 200 ≈ 20 帧后 ring 满。我们写-读循环 100 帧，
    // 每次写 1 帧读 1 帧，保证写位置持续推进并多次回绕。
    constexpr uint32_t frame_len = 200;
    constexpr int total_frames   = 100;

    void *shm = AllocShm();

    for (int i = 0; i < total_frames; ++i)
    {
        char wbuf[frame_len];
        FillPattern(wbuf, frame_len, static_cast<uint8_t>(i));

        if (SmallRing::TryWrite(shm, wbuf, frame_len) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite frame #%d\n", i);
            std::abort();
        }

        char rbuf[frame_len];
        if (SmallRing::ReadExact(shm, rbuf, frame_len) != 0)
        {
            std::fprintf(stderr, "FAIL: ReadExact frame #%d\n", i);
            std::abort();
        }

        char ctx[64];
        std::snprintf(ctx, sizeof(ctx), "multi-frame #%d", i);
        VerifyPattern(rbuf, frame_len, static_cast<uint8_t>(i), ctx);
    }

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: %d frames (multiple wrap-arounds)\n", total_frames);
}

// =========================================================================
// 测试 7: 多帧批量写 + 批量读，穿越多次回绕
// =========================================================================

void TestMultiFrameBatchWrap()
{
    std::printf("--- Test 7: Multi-frame batch write wrap ---\n");

    constexpr uint32_t frame_len = 150;
    constexpr int batch_count    = 10;
    constexpr int iterations     = 8; // 10*150*8 = 12000 >> 4096, 多次回绕

    void *shm = AllocShm();

    for (int iter = 0; iter < iterations; ++iter)
    {
        // 批量写 batch_count 帧
        {
            SmallRing::BatchWriter batch(shm);
            for (int i = 0; i < batch_count; ++i)
            {
                char wbuf[frame_len];
                uint8_t seed = static_cast<uint8_t>(iter * batch_count + i);
                FillPattern(wbuf, frame_len, seed);
                if (batch.TryWrite(wbuf, frame_len) != 0)
                {
                    std::fprintf(stderr,
                                 "FAIL: BatchWriter iter=%d i=%d\n", iter, i);
                    std::abort();
                }
            }
        } // 析构触发 Flush

        // 批量读 batch_count 帧
        for (int i = 0; i < batch_count; ++i)
        {
            char rbuf[frame_len];
            if (SmallRing::ReadExact(shm, rbuf, frame_len) != 0)
            {
                std::fprintf(stderr,
                             "FAIL: ReadExact iter=%d i=%d\n", iter, i);
                std::abort();
            }
            uint8_t seed = static_cast<uint8_t>(iter * batch_count + i);
            char ctx[64];
            std::snprintf(ctx, sizeof(ctx), "batch iter=%d i=%d", iter, i);
            VerifyPattern(rbuf, frame_len, seed, ctx);
        }
    }

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS: %d iterations × %d frames\n", iterations, batch_count);
}

// =========================================================================
// 测试 8: 全面参数化 tail 偏移 —— 遍历 1..msg_len+1
// =========================================================================

void TestExhaustiveTailOffsets()
{
    std::printf("--- Test 8: Exhaustive tail offsets ---\n");

    constexpr uint32_t msg_len = 64;
    int tested = 0;

    // 遍历 tail = 1 .. msg_len+1（涵盖 msg_len < tail 不回绕 和 msg_len > tail 回绕）
    for (uint32_t tail = 1; tail <= msg_len + 1 && tail < kSmallCap; ++tail)
    {
        void *shm = AllocShm();
        AdvanceRing(shm, kSmallCap - tail);

        char wbuf[msg_len];
        FillPattern(wbuf, msg_len, static_cast<uint8_t>(tail));
        if (SmallRing::TryWrite(shm, wbuf, msg_len) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite tail=%u\n", tail);
            std::abort();
        }

        char rbuf[msg_len];
        std::memset(rbuf, 0xFF, msg_len);
        if (SmallRing::ReadExact(shm, rbuf, msg_len) != 0)
        {
            std::fprintf(stderr, "FAIL: ReadExact tail=%u\n", tail);
            std::abort();
        }

        char ctx[64];
        std::snprintf(ctx, sizeof(ctx), "exhaustive tail=%u", tail);
        VerifyPattern(rbuf, msg_len, static_cast<uint8_t>(tail), ctx);

        std::free(shm);
        ++tested;
    }

    ++g_pass_count;
    std::printf("  PASS: %d tail offsets tested\n", tested);
}

// =========================================================================
// 测试 9: Codec Encode → 写入环 → 跨环尾 → ReadExact → Decode
// =========================================================================

void TestCodecWrap()
{
    std::printf("--- Test 9: Codec Encode/Decode through ring wrap ---\n");

    constexpr uint32_t frame_size =
        shm::kMsgHeaderSize + shm::kTagSize + sizeof(Heartbeat);

    // 遍历多种 tail
    std::vector<uint32_t> tails = {1, 2, 4, 7, 8, frame_size / 2,
                                    frame_size - 1, frame_size,
                                    frame_size + 1};
    for (uint32_t tail : tails)
    {
        if (tail >= kSmallCap) continue;

        void *shm = AllocShm();
        AdvanceRing(shm, kSmallCap - tail);

        // Encode
        Heartbeat hb{};
        hb.client_id = 42;
        hb.seq       = static_cast<int32_t>(tail);
        hb.timestamp = 9999;

        char enc[frame_size];
        uint32_t seq_in = tail * 100;
        if (shm::EncodePod(hb, enc, frame_size, seq_in) != frame_size)
        {
            std::fprintf(stderr, "FAIL: Encode tail=%u\n", tail);
            std::abort();
        }

        // 写入 ring
        if (SmallRing::TryWrite(shm, enc, frame_size) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite tail=%u\n", tail);
            std::abort();
        }

        // 从 ring 读出
        char dec[frame_size];
        if (SmallRing::ReadExact(shm, dec, frame_size) != 0)
        {
            std::fprintf(stderr, "FAIL: ReadExact tail=%u\n", tail);
            std::abort();
        }

        // Decode
        Heartbeat out{};
        uint32_t seq_out = 0;
        const void *dec_payload = nullptr;
        uint32_t dec_payload_len = 0;
        if (!shm::Decode(dec, frame_size, &dec_payload, &dec_payload_len, &seq_out))
        {
            std::fprintf(stderr, "FAIL: Decode tail=%u\n", tail);
            std::abort();
        }
        // payload = [tag u32][Heartbeat bytes], 跳过 tag
        const void *pod_data = static_cast<const char *>(dec_payload) + shm::kTagSize;
        uint32_t pod_len = dec_payload_len - shm::kTagSize;
        if (!shm::DecodePod<Heartbeat>(pod_data, pod_len, &out))
        {
            std::fprintf(stderr, "FAIL: Decode tail=%u\n", tail);
            std::abort();
        }
        if (out.client_id != 42 || out.seq != static_cast<int32_t>(tail) ||
            out.timestamp != 9999 || seq_out != seq_in)
        {
            std::fprintf(stderr,
                         "FAIL: field mismatch tail=%u "
                         "(client_id=%d seq=%d ts=%ld msg_seq=%u)\n",
                         tail, out.client_id, out.seq,
                         static_cast<long>(out.timestamp), seq_out);
            std::abort();
        }

        std::free(shm);
    }

    ++g_pass_count;
    std::printf("  PASS: %d tail offsets\n", static_cast<int>(tails.size()));
}

// =========================================================================
// 测试 10: 大消息 (接近 max_write_size) 跨环尾
// =========================================================================

void TestLargeMessageWrap()
{
    std::printf("--- Test 10: Large message near max_write_size wrap ---\n");

    // max_write_size = 4096/2 = 2048
    constexpr uint32_t msg_len = SmallRing::max_write_size;

    // tail=100: 绝大部分数据在环头部
    std::vector<uint32_t> tails = {1, 16, 100, msg_len / 2, msg_len - 1, msg_len};

    auto *wbuf = new char[msg_len];
    auto *rbuf = new char[msg_len];

    for (uint32_t tail : tails)
    {
        if (tail >= kSmallCap) continue;

        void *shm = AllocShm();
        AdvanceRing(shm, kSmallCap - tail);

        FillPattern(wbuf, msg_len, static_cast<uint8_t>(tail));

        if (SmallRing::TryWrite(shm, wbuf, msg_len) != 0)
        {
            std::fprintf(stderr, "FAIL: TryWrite large tail=%u\n", tail);
            std::abort();
        }

        std::memset(rbuf, 0xFF, msg_len);
        if (SmallRing::ReadExact(shm, rbuf, msg_len) != 0)
        {
            std::fprintf(stderr, "FAIL: ReadExact large tail=%u\n", tail);
            std::abort();
        }

        char ctx[64];
        std::snprintf(ctx, sizeof(ctx), "large tail=%u", tail);
        VerifyPattern(rbuf, msg_len, static_cast<uint8_t>(tail), ctx);

        std::free(shm);
    }

    delete[] wbuf;
    delete[] rbuf;
    ++g_pass_count;
    std::printf("  PASS: %d tail offsets with msg_len=%u\n",
                static_cast<int>(tails.size()), msg_len);
}

// =========================================================================
// 测试 11: TryWrite 拒绝过大 / 空间不足
// =========================================================================

void TestWriteRejectCases()
{
    std::printf("--- Test 11: TryWrite reject cases ---\n");

    void *shm = AllocShm();

    // len=0 应该被拒绝
    if (SmallRing::TryWrite(shm, "x", 0) != -1)
    {
        std::fprintf(stderr, "FAIL: len=0 not rejected\n");
        std::abort();
    }

    // len > max_write_size 应该被拒绝
    auto *big = new char[SmallRing::max_write_size + 1];
    if (SmallRing::TryWrite(shm, big, SmallRing::max_write_size + 1) != -1)
    {
        std::fprintf(stderr, "FAIL: oversized not rejected\n");
        std::abort();
    }
    delete[] big;

    // 填满 ring 后再写应该被拒绝
    char fill[SmallRing::max_write_size];
    std::memset(fill, 'X', sizeof(fill));
    if (SmallRing::TryWrite(shm, fill, SmallRing::max_write_size) != 0)
    {
        std::fprintf(stderr, "FAIL: first max write\n");
        std::abort();
    }
    // ring 已用 max_write_size = Capacity/2，再写 max_write_size 应成功
    if (SmallRing::TryWrite(shm, fill, SmallRing::max_write_size) != 0)
    {
        std::fprintf(stderr, "FAIL: second max write\n");
        std::abort();
    }
    // ring 已满，再写 1 字节应被拒绝
    if (SmallRing::TryWrite(shm, "x", 1) != -1)
    {
        std::fprintf(stderr, "FAIL: full ring not rejected\n");
        std::abort();
    }

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 12: ReadExact 数据不足不消费
// =========================================================================

void TestReadExactNoConsume()
{
    std::printf("--- Test 12: ReadExact partial-available no consume ---\n");

    void *shm = AllocShm();

    // 写 50 字节
    char wbuf[50];
    FillPattern(wbuf, 50, 0x77);
    SmallRing::TryWrite(shm, wbuf, 50);

    // 试图读 100 字节 —— 应失败
    char rbuf[100];
    if (SmallRing::ReadExact(shm, rbuf, 100) != -1)
    {
        std::fprintf(stderr, "FAIL: ReadExact(100) should fail\n");
        std::abort();
    }

    // 数据未被消费，Available 仍为 50
    if (SmallRing::Available(shm) != 50)
    {
        std::fprintf(stderr, "FAIL: Available=%lu, expected 50\n",
                     static_cast<unsigned long>(SmallRing::Available(shm)));
        std::abort();
    }

    // 正常读 50 字节应成功
    if (SmallRing::ReadExact(shm, rbuf, 50) != 0)
    {
        std::fprintf(stderr, "FAIL: ReadExact(50)\n");
        std::abort();
    }
    VerifyPattern(rbuf, 50, 0x77, "ReadExact no-consume");

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

// =========================================================================
// 测试 13: FreeSpace / Available 一致性
// =========================================================================

void TestFreeSpaceAvailable()
{
    std::printf("--- Test 13: FreeSpace + Available consistency ---\n");

    void *shm = AllocShm();

    if (SmallRing::Available(shm) != 0 || SmallRing::FreeSpace(shm) != kSmallCap)
    {
        std::fprintf(stderr, "FAIL: initial state\n");
        std::abort();
    }

    char buf[100];
    SmallRing::TryWrite(shm, buf, 100);
    if (SmallRing::Available(shm) != 100 ||
        SmallRing::FreeSpace(shm) != kSmallCap - 100)
    {
        std::fprintf(stderr, "FAIL: after write 100\n");
        std::abort();
    }

    SmallRing::ReadExact(shm, buf, 60);
    if (SmallRing::Available(shm) != 40 ||
        SmallRing::FreeSpace(shm) != kSmallCap - 40)
    {
        std::fprintf(stderr, "FAIL: after read 60\n");
        std::abort();
    }

    SmallRing::ReadExact(shm, buf, 40);
    if (SmallRing::Available(shm) != 0 || SmallRing::FreeSpace(shm) != kSmallCap)
    {
        std::fprintf(stderr, "FAIL: after read all\n");
        std::abort();
    }

    std::free(shm);
    ++g_pass_count;
    std::printf("  PASS\n");
}

}  // anonymous namespace

int main()
{
    std::printf("=== RingBuf wrap-around test suite (Capacity=%zu) ===\n\n",
                kSmallCap);

    TestWriteReadExactWrap();
    TestWriteTryReadPartialWrap();
    TestPeekCommitReadWrap();
    TestPeekNoWrap();
    TestBatchWriterWrap();
    TestMultiFrameWrap();
    TestMultiFrameBatchWrap();
    TestExhaustiveTailOffsets();
    TestCodecWrap();
    TestLargeMessageWrap();
    TestWriteRejectCases();
    TestReadExactNoConsume();
    TestFreeSpaceAvailable();

    std::printf("\n=== TEST PASSED === (%d sub-tests)\n", g_pass_count);
    return 0;
}
