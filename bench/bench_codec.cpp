/**
 * @file bench_codec.cpp
 * @brief Codec 编解码基准测试
 *
 * 测试维度：
 * 1. Encode + Decode 内存往返（纯 CPU，不涉及 ring）
 * 2. EncodePod + DecodePod 内存往返
 * 3. Send + FrameReader 跨进程（经 RingChannel，含通知）
 * 4. SendPod + FrameReader 跨进程
 * 5. Send vs 裸 TryWrite 开销对比（量化 codec 层 overhead）
 *
 * 使用 fork() + socketpair()，写端计时，读端忙轮询消费。
 */

#include <shm_ipc/codec.hpp>
#include <shm_ipc/messages.hpp>
#include <shm_ipc/ring_channel.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

/// 阻止编译器优化掉对 val 的读写
template <typename T>
inline void DoNotOptimize(T const &val)
{
    asm volatile("" : : "r,m"(val) : "memory");
}

using Channel = shm_ipc::DefaultRingChannel;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

uint64_t NowNs()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// 测试参数
// ---------------------------------------------------------------------------

struct TestCase
{
    uint32_t payload_size;  ///< payload 字节数
    int      rounds;        ///< 测试轮次
};

/// 跨进程 ring 测试用例（不含大消息，受 eventfd 系统调用瓶颈限制）
constexpr std::array<TestCase, 4> kTests = {{
    {     64, 500000},
    {    256, 500000},
    {   1024, 200000},
    {   4096,  50000},
}};

/// 纯内存 Encode/Decode 测试用例（含大消息）
constexpr std::array<TestCase, 5> kMemTests = {{
    {     64, 500000},
    {    256, 500000},
    {   1024, 200000},
    {   4096, 100000},
    {  65536,  20000},
}};

constexpr int kRepeat = 3;     ///< 每组重复取最小值
constexpr int kWarmup = 500;   ///< 预热轮次

constexpr uint32_t kTag = 0xBEEF;

// ---------------------------------------------------------------------------
// 测试 1 & 2：纯内存 Encode/Decode（不涉及 ring，单进程）
// ---------------------------------------------------------------------------

struct MemoryResult
{
    uint32_t payload_size;
    double   encode_ns;       ///< Encode 每次平均 ns
    double   decode_ns;       ///< Decode 每次平均 ns
    double   encode_pod_ns;   ///< EncodePod 每次平均 ns
    double   decode_pod_ns;   ///< DecodePod 每次平均 ns
};

void BenchMemory()
{
    std::printf("=== Bench 1 & 2: Encode/Decode in-memory ===\n\n");
    std::printf("%12s %10s %12s %12s %14s %14s\n",
                "payload", "rounds", "Encode(ns)", "Decode(ns)",
                "EncodePod(ns)", "DecodePod(ns)");

    // EncodePod/DecodePod 用 ClientMsg
    constexpr uint32_t pod_frame_size =
        shm_ipc::kMsgHeaderSize + shm_ipc::kTagSize + sizeof(ClientMsg);

    for (auto &tc : kMemTests)
    {
        uint32_t frame_size = shm_ipc::kMsgHeaderSize + shm_ipc::kTagSize + tc.payload_size;
        auto buf = std::make_unique<char[]>(frame_size);
        auto payload = std::make_unique<char[]>(tc.payload_size);
        std::memset(payload.get(), 'P', tc.payload_size);

        // --- Encode ---
        double best_enc = 1e18;
        for (int rep = 0; rep < kRepeat; ++rep)
        {
            uint64_t t0 = NowNs();
            for (int i = 0; i < tc.rounds; ++i)
            {
                auto n = shm_ipc::Encode(kTag, payload.get(), tc.payload_size,
                                buf.get(), frame_size, static_cast<uint32_t>(i));
                DoNotOptimize(n);
            }
            double avg = static_cast<double>(NowNs() - t0) / tc.rounds;
            best_enc = std::min(best_enc, avg);
        }

        // --- Decode ---
        // 先编码一帧用于解码
        shm_ipc::Encode(kTag, payload.get(), tc.payload_size,
                        buf.get(), frame_size, 0);

        double best_dec = 1e18;
        for (int rep = 0; rep < kRepeat; ++rep)
        {
            uint32_t out_tag = 0;
            const void *out_payload = nullptr;
            uint32_t out_len = 0;
            uint32_t out_seq = 0;

            uint64_t t0 = NowNs();
            for (int i = 0; i < tc.rounds; ++i)
            {
                bool ok = shm_ipc::Decode(buf.get(), frame_size,
                                &out_tag, &out_payload, &out_len, &out_seq);
                DoNotOptimize(ok);
                DoNotOptimize(out_len);
            }
            double avg = static_cast<double>(NowNs() - t0) / tc.rounds;
            best_dec = std::min(best_dec, avg);
        }

        // --- EncodePod (ClientMsg, fixed size) ---
        double best_epod = 1e18;
        if (tc.payload_size <= sizeof(ClientMsg))
        {
            auto pod_buf = std::make_unique<char[]>(pod_frame_size);
            ClientMsg msg{};
            msg.payload_len = tc.payload_size;
            std::memset(msg.payload, 'C', tc.payload_size);

            for (int rep = 0; rep < kRepeat; ++rep)
            {
                uint64_t t0 = NowNs();
                for (int i = 0; i < tc.rounds; ++i)
                {
                    msg.seq = i;
                    auto n = shm_ipc::EncodePod(msg, pod_buf.get(), pod_frame_size,
                                       static_cast<uint32_t>(i));
                    DoNotOptimize(n);
                }
                double avg = static_cast<double>(NowNs() - t0) / tc.rounds;
                best_epod = std::min(best_epod, avg);
            }

            // --- DecodePod ---
            shm_ipc::EncodePod(msg, pod_buf.get(), pod_frame_size, 0);
            uint32_t out_tag = 0;
            const void *out_payload = nullptr;
            uint32_t out_len = 0;
            shm_ipc::Decode(pod_buf.get(), pod_frame_size,
                            &out_tag, &out_payload, &out_len);

            double best_dpod = 1e18;
            for (int rep = 0; rep < kRepeat; ++rep)
            {
                ClientMsg out{};
                uint64_t t0 = NowNs();
                for (int i = 0; i < tc.rounds; ++i)
                {
                    bool ok = shm_ipc::DecodePod<ClientMsg>(out_payload, out_len, &out);
                    DoNotOptimize(ok);
                    DoNotOptimize(out.seq);
                }
                double avg = static_cast<double>(NowNs() - t0) / tc.rounds;
                best_dpod = std::min(best_dpod, avg);
            }

            std::printf("%12u %10d %12.1f %12.1f %14.1f %14.1f\n",
                        tc.payload_size, tc.rounds,
                        best_enc, best_dec, best_epod, best_dpod);
        }
        else
        {
            std::printf("%12u %10d %12.1f %12.1f %14s %14s\n",
                        tc.payload_size, tc.rounds,
                        best_enc, best_dec, "-", "-");
        }
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// 测试 3 & 4 & 5：跨进程 Send/SendPod + FrameReader（经 ring）
// ---------------------------------------------------------------------------

/// 读端控制命令
struct ReadCmd
{
    int32_t  count;     ///< 待消费帧数（0 = 退出）
    uint32_t msg_len;   ///< 0 = 用 FrameReader 拆帧, >0 = 裸 ReadExact
};

// --- 读端 ---

void RunReader(int socket_fd, int ctrl_fd)
{
    auto ch  = Channel::Accept(socket_fd);
    auto raw_buf = std::make_unique<char[]>(Channel::max_write_size);

    shm_ipc::FrameReader<> reader;

    for (;;)
    {
        ReadCmd cmd{};
        if (::read(ctrl_fd, &cmd, sizeof(cmd)) != sizeof(cmd))
            break;
        if (cmd.count <= 0)
            break;

        if (cmd.msg_len > 0)
        {
            // 裸 ReadExact 模式
            for (int32_t i = 0; i < cmd.count; ++i)
            {
                while (ch.ReadExact(raw_buf.get(), cmd.msg_len) != 0)
                    ;
            }
        }
        else
        {
            // FrameReader 拆帧模式
            int32_t received = 0;
            uint32_t tag = 0;
            const void *payload = nullptr;
            uint32_t payload_len = 0;
            while (received < cmd.count)
            {
                if (reader.TryRecv(ch, &tag, &payload, &payload_len) == 0)
                    ++received;
            }
        }

        char ack = 1;
        (void)::write(ctrl_fd, &ack, 1);
    }

    ::close(ctrl_fd);
    ::close(socket_fd);
    std::_Exit(0);
}

// --- 写端辅助 ---

void RequestRead(int ctrl_fd, int32_t count, uint32_t msg_len)
{
    ReadCmd cmd{count, msg_len};
    (void)::write(ctrl_fd, &cmd, sizeof(cmd));
}

void WaitReadDone(int ctrl_fd)
{
    char ack = 0;
    (void)::read(ctrl_fd, &ack, 1);
}

// --- 写端 ---

struct RingResult
{
    uint32_t payload_size;
    int      rounds;
    double   send_ns;       ///< Send 每次 ns
    double   send_pod_ns;   ///< SendPod 每次 ns（仅 ClientMsg 适用时）
    double   raw_ns;        ///< 裸 TryWrite 每次 ns（基线）
};

void RunWriter(int socket_fd, int ctrl_fd)
{
    auto ch = Channel::Connect(socket_fd);
    auto payload = std::make_unique<char[]>(Channel::max_write_size);
    std::memset(payload.get(), 'W', Channel::max_write_size);

    std::vector<RingResult> results;

    std::fprintf(stderr, "running %zu codec ring benchmarks, %d repeats ...\n",
                 kTests.size(), kRepeat);

    for (auto &tc : kTests)
    {
        uint32_t frame_size = shm_ipc::kMsgHeaderSize + shm_ipc::kTagSize + tc.payload_size;

        // ---- 裸 TryWrite 基线 ----
        double best_raw = 1e18;
        for (int rep = 0; rep < kRepeat; ++rep)
        {
            // 预热
            RequestRead(ctrl_fd, kWarmup, frame_size);
            for (int i = 0; i < kWarmup; ++i)
                while (ch.TryWrite(payload.get(), frame_size) != 0)
                    ;
            WaitReadDone(ctrl_fd);

            // 正式
            RequestRead(ctrl_fd, tc.rounds, frame_size);
            uint64_t t0 = NowNs();
            for (int i = 0; i < tc.rounds; ++i)
                while (ch.TryWrite(payload.get(), frame_size) != 0)
                    ;
            uint64_t elapsed = NowNs() - t0;
            WaitReadDone(ctrl_fd);
            best_raw = std::min(best_raw, static_cast<double>(elapsed) / tc.rounds);
        }

        // ---- Send (generic, FrameReader 拆帧) ----
        double best_send = 1e18;
        for (int rep = 0; rep < kRepeat; ++rep)
        {
            // 预热
            RequestRead(ctrl_fd, kWarmup, 0);
            for (int i = 0; i < kWarmup; ++i)
                while (shm_ipc::Send(ch, kTag, payload.get(), tc.payload_size,
                                     static_cast<uint32_t>(i)) != 0)
                    ;
            WaitReadDone(ctrl_fd);

            // 正式
            RequestRead(ctrl_fd, tc.rounds, 0);
            uint64_t t0 = NowNs();
            for (int i = 0; i < tc.rounds; ++i)
                while (shm_ipc::Send(ch, kTag, payload.get(), tc.payload_size,
                                     static_cast<uint32_t>(i)) != 0)
                    ;
            uint64_t elapsed = NowNs() - t0;
            WaitReadDone(ctrl_fd);
            best_send = std::min(best_send, static_cast<double>(elapsed) / tc.rounds);
        }

        // ---- SendPod (ClientMsg, FrameReader 拆帧) ----
        double best_pod = -1;
        if (tc.payload_size <= sizeof(ClientMsg::payload))
        {
            ClientMsg msg{};
            msg.payload_len = tc.payload_size;
            std::memset(msg.payload, 'C', tc.payload_size);

            best_pod = 1e18;
            for (int rep = 0; rep < kRepeat; ++rep)
            {
                RequestRead(ctrl_fd, kWarmup, 0);
                for (int i = 0; i < kWarmup; ++i)
                {
                    msg.seq = i;
                    while (shm_ipc::SendPod(ch, msg, static_cast<uint32_t>(i)) != 0)
                        ;
                }
                WaitReadDone(ctrl_fd);

                RequestRead(ctrl_fd, tc.rounds, 0);
                uint64_t t0 = NowNs();
                for (int i = 0; i < tc.rounds; ++i)
                {
                    msg.seq = i;
                    while (shm_ipc::SendPod(ch, msg, static_cast<uint32_t>(i)) != 0)
                        ;
                }
                uint64_t elapsed = NowNs() - t0;
                WaitReadDone(ctrl_fd);
                best_pod = std::min(best_pod, static_cast<double>(elapsed) / tc.rounds);
            }
        }

        results.push_back({tc.payload_size, tc.rounds, best_send, best_pod, best_raw});

        std::fprintf(stderr, "  payload=%5uB  raw=%7.0f ns  Send=%7.0f ns  SendPod=%7.0f ns\n",
                     tc.payload_size, best_raw, best_send,
                     best_pod > 0 ? best_pod : 0.0);
    }

    // 通知读端退出
    ReadCmd quit{0, 0};
    (void)::write(ctrl_fd, &quit, sizeof(quit));

    // ---- 输出报告 ----

    std::printf("=== Bench 3 & 4 & 5: Send/SendPod + FrameReader cross-process ===\n\n");

    // 表 1：绝对延迟
    std::printf("### 每次操作平均延迟\n\n");
    std::printf("%12s %10s %12s %12s %14s %14s\n",
                "payload", "rounds", "raw(ns)", "Send(ns)", "SendPod(ns)", "overhead(ns)");

    for (auto &r : results)
    {
        if (r.send_pod_ns > 0)
        {
            std::printf("%12u %10d %12.0f %12.0f %14.0f %14.0f\n",
                        r.payload_size, r.rounds,
                        r.raw_ns, r.send_ns, r.send_pod_ns,
                        r.send_ns - r.raw_ns);
        }
        else
        {
            std::printf("%12u %10d %12.0f %12.0f %14s %14.0f\n",
                        r.payload_size, r.rounds,
                        r.raw_ns, r.send_ns, "-",
                        r.send_ns - r.raw_ns);
        }
    }

    // 表 2：吞吐量
    std::printf("\n### 吞吐量\n\n");
    std::printf("%12s %14s %14s %14s\n",
                "payload", "raw(MB/s)", "Send(MB/s)", "SendPod(MB/s)");

    for (auto &r : results)
    {
        uint32_t total_bytes = shm_ipc::kMsgHeaderSize + shm_ipc::kTagSize + r.payload_size;
        double raw_mbs  = static_cast<double>(total_bytes) / r.raw_ns * 1e9 / (1024.0 * 1024.0);
        double send_mbs = static_cast<double>(total_bytes) / r.send_ns * 1e9 / (1024.0 * 1024.0);

        if (r.send_pod_ns > 0)
        {
            uint32_t pod_bytes = shm_ipc::kMsgHeaderSize + shm_ipc::kTagSize + sizeof(ClientMsg);
            double pod_mbs = static_cast<double>(pod_bytes) / r.send_pod_ns * 1e9 / (1024.0 * 1024.0);
            std::printf("%12u %14.0f %14.0f %14.0f\n",
                        r.payload_size, raw_mbs, send_mbs, pod_mbs);
        }
        else
        {
            std::printf("%12u %14.0f %14.0f %14s\n",
                        r.payload_size, raw_mbs, send_mbs, "-");
        }
    }

    // 表 3：codec 开销占比
    std::printf("\n### Codec 开销占比（Send vs 裸 TryWrite）\n\n");
    std::printf("%12s %14s %14s %14s\n",
                "payload", "raw(ns)", "Send(ns)", "overhead%%");

    for (auto &r : results)
    {
        double pct = (r.send_ns - r.raw_ns) / r.raw_ns * 100.0;
        std::printf("%12u %14.0f %14.0f %13.1f%%\n",
                    r.payload_size, r.raw_ns, r.send_ns, pct);
    }

    std::printf("\n");

    ::close(ctrl_fd);
    ::close(socket_fd);
}

}  // anonymous namespace

int main()
{
    // 先跑纯内存测试（单进程）
    BenchMemory();

    // 再跑跨进程 ring 测试
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        std::perror("socketpair");
        return 1;
    }

    int ctrl[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ctrl) < 0)
    {
        std::perror("socketpair(ctrl)");
        return 1;
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        std::perror("fork");
        return 1;
    }

    if (pid == 0)
    {
        ::close(sv[0]);
        ::close(ctrl[0]);
        RunReader(sv[1], ctrl[1]);
    }
    else
    {
        ::close(sv[1]);
        ::close(ctrl[1]);
        RunWriter(sv[0], ctrl[0]);
        ::waitpid(pid, nullptr, 0);
    }

    return 0;
}
