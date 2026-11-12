/**
 * @file shm_connection.hpp
 * @brief 共享内存连接实现（C++17）：shm::ShmConnection 实现 comm::ITcpConnection
 *
 * 将「Unix socket + RingChannel + codec」封装为单条连接对象，
 * 由 shm::ShmServer / shm::ShmAsyncClient 创建并驱动，应用层不直接构造。
 *
 * 应用层只依赖 comm::ITcpConnection 接口：
 * @code
 * conn->SendCmd(pkg);                 // std::any 消息，codec 编码后写入环
 * conn->Close();                      // 主动关闭（异步收尾，触发 close 回调）
 * conn->SetContext(ctx);
 * @endcode
 *
 * shm 实现的特点：
 * - 数据面是共享内存字节流（与 TCP 流语义一致：数据不完整时等待对端续写）
 * - Unix socket 仅承担握手与断连检测（对端退出 = socket EOF）
 * - StartVerifyTimer 的 interval_ms 参数暂不支持（一次性触发）
 */

#ifndef SHM_IPC_SHM_CONNECTION_HPP_
#define SHM_IPC_SHM_CONNECTION_HPP_

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <comm/codec.h>
#include <comm/connection.h>
#include <comm/types.h>

#include "common.hpp"
#include "event_loop.hpp"
#include "ring_channel.hpp"

namespace shm {

/**
 * @brief 共享内存连接（实现 comm::ITcpConnection，对应框架 tcp::TcpConnection）
 *
 * @thread-safety 非线程安全：SendCmd / Close / 回调均限定在所属 EventLoop 线程；
 *                底层 SPSC 环每个方向只允许一个生产者线程和一个消费者线程。
 */
class ShmConnection : public comm::ITcpConnection,
                      public std::enable_shared_from_this<ShmConnection>
{
 public:
    /**
     * @brief 构造连接（由 ShmServer / ShmAsyncClient 在握手成功后调用）
     * @param loop      事件循环（StartVerifyTimer 使用）
     * @param id        连接 ID
     * @param socket_fd 已建立连接的 Unix socket fd
     * @param channel   握手完成的双向环形通道
     */
    ShmConnection(EventLoop* loop, uint64_t id, UniqueFd socket_fd, DefaultRingChannel channel)
        : loop_(loop), id_(id), socket_fd_(std::move(socket_fd)), channel_(std::move(channel)), encode_buf_(128 * 1024)
    {}

    ~ShmConnection() override = default;

    // 禁止复制和移动（事件回调通过 shared_ptr 引用本对象）
    ShmConnection(const ShmConnection&)            = delete;
    ShmConnection& operator=(const ShmConnection&) = delete;
    ShmConnection(ShmConnection&&)                 = delete;
    ShmConnection& operator=(ShmConnection&&)      = delete;

    // ------------------------------------------------------------------
    // comm::ITcpConnection 接口实现
    // ------------------------------------------------------------------

    /**
     * @brief 发送消息包
     *
     * codec->Encode 编码为字节流后写入共享内存环并通知对端。
     * 返回值约定与框架 Encode 一致：
     * - >0（成功编码）时写入环，返回 true
     * - kCodecSkipPackage：丢包，返回 true（同框架，不算失败）
     * - kCodecDisconnect：主动 Close() 并返回 false
     * - kCodecNeedMoreBuffer：按 *size 扩缓冲区重试，超过上限返回 false
     * - 环空间不足：返回 false（不阻塞；框架行为是断开连接，这里交由调用方决策）
     */
    bool SendCmd(const std::any& pkg) override
    {
        if (state_ != comm::TcpConnStates::established && state_ != comm::TcpConnStates::verifing)
        {
            return false;
        }
        if (!codec_)
        {
            return false;
        }

        try
        {
            // 快速路径：codec 能报告精确编码长度时，直接在 ring 中预留
            // 空间并编码，把 "any → encode_buf_ → ring" 两次拷贝降为一次。
            // 小消息继续走 encode_buf_（缓存命中，避免 Reserve/Commit 开销）。
            const int32_t need = codec_->EncodedSize(pkg);
            if (need >= static_cast<int32_t>(kDirectEncodeMinBytes) &&
                static_cast<uint32_t>(need) <= DefaultRingChannel::kMaxWriteSize)
            {
                DefaultRingChannel::ChannelBatchWriter batch;
                if (channel_.StartBatch(&batch) == ErrorCode::kOk)
                {
                    char* dst = batch.Reserve(static_cast<uint32_t>(need));
                    if (!dst)
                    {
                        return false;  // 环空间不足（与 WriteRaw 预检一致）
                    }

                    size_t out = 0;
                    const int32_t len =
                        codec_->Encode(pkg, reinterpret_cast<comm::byte*>(dst), static_cast<size_t>(need), &out);
                    if (len == need)
                    {
                        batch.CommitReserve(static_cast<uint32_t>(len));
                        batch.Flush();  // 发布 write_pos 并按需通知对端
                        return true;
                    }

                    // 预留不足/长度不符：回滚后走原有 encode_buf_ 路径
                    batch.Cancel();
                    if (len == comm::kCodecSkipPackage)
                    {
                        return true;
                    }
                    if (len == comm::kCodecDisconnect)
                    {
                        Close();
                        return false;
                    }
                }
            }

            for (;;)
            {
                size_t out = 0;
                const int32_t len =
                    codec_->Encode(pkg, reinterpret_cast<comm::byte*>(encode_buf_.data()), encode_buf_.size(), &out);
                if (len == comm::kCodecSkipPackage)
                {
                    return true;  // 丢包（同框架）
                }
                if (len == comm::kCodecDisconnect)
                {
                    Close();
                    return false;
                }
                if (len == comm::kCodecNeedMoreBuffer)
                {
                    // out 为所需大小：为 0（内部错误）或不超过现有缓冲仍要求
                    // 扩容（codec 违约）时放弃，避免无限重试
                    if (out == 0 || out <= encode_buf_.size() || !GrowEncodeBuffer(out))
                    {
                        return false;
                    }
                    continue;
                }
                return WriteRaw(encode_buf_.data(), static_cast<uint32_t>(len));
            }
        }
        catch (const std::bad_alloc&)
        {
            return false;  // 编码缓冲扩容失败：本包发送失败
        }
    }

    /**
     * @brief 主动关闭连接
     *
     * 进入 closeing 状态并 shutdown socket（对端将收到 EOF）。
     * 实际的 fd 移除与 close 回调由 owner（server/client）异步完成，
     * 之后 GetCloseType() 返回 CloseType::Active。
     */
    void Close() override
    {
        if (state_ == comm::TcpConnStates::established || state_ == comm::TcpConnStates::verifing)
        {
            close_type_ = comm::CloseType::Active;
            state_      = comm::TcpConnStates::closeing;
            ::shutdown(socket_fd_.Get(), SHUT_RDWR);
        }
        RemoveVerifyTimer();
    }

    int32_t State() const override { return state_; }
    uint64_t Id() const override { return id_; }

    /**
     * @brief 对端地址（Unix socket 无对端地址，返回 "pid:<n>" 形式的对端进程号）
     */
    std::string RemoteAddr() const override
    {
        ucred cred{};
        socklen_t len = sizeof(cred);
        if (::getsockopt(socket_fd_.Get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
        {
            return "pid:" + std::to_string(cred.pid);
        }
        return {};
    }

    comm::CloseType GetCloseType() const override { return close_type_; }

    void SetContext(std::any context) override { context_ = std::move(context); }
    const std::any& Context() const override { return context_; }
    void SetRpcContext(std::any context) override { rpc_context_ = std::move(context); }
    const std::any& RpcContext() const override { return rpc_context_; }

    /**
     * @brief 启动验证超时定时器
     *
     * @param expired_ms  超时时间（毫秒），一次性触发
     * @param interval_ms 暂不支持（shm 版本为一次性定时器），传 0
     * @param action      超时回调
     */
    void StartVerifyTimer(int64_t expired_ms, int64_t interval_ms, comm::TimeAction action) override
    {
        if (verify_timer_)
        {
            return;
        }
        (void)interval_ms;  // 一次性定时器，不支持重复触发

        int tfd            = 0;
        const ErrorCode ec = loop_->AddOneShotTimer(
            static_cast<int>(expired_ms),
            [this, action = std::move(action)](int, short) mutable {
                verify_timer_ = 0;  // 定时器已自行移除
                action(0);
            },
            &tfd);
        if (ec == ErrorCode::kOk)
        {
            verify_timer_ = tfd;
        }
        else
        {
            Close();  // 无法挂载超时定时器：fail-closed（避免验证永不超时）
        }
    }

    /**
     * @brief 开启/关闭应用层心跳响应
     *
     * 开启后，解码方向收到心跳包（codec->Decode 返回 -2）时，
     * 自动调用 codec->SendKeepAlive 生成响应包回发给对端。
     */
    void SetKeepAlive(bool keepalive) override { keepalive_ = keepalive; }

    void ResizeBuffer(size_t size, size_t max) override
    {
        max_buffer_size_ = max;
        if (encode_buf_.size() < size)
        {
            encode_buf_.resize(size);
        }
    }

    comm::codec::CodecPtr GetCodec() override { return codec_; }

    // ------------------------------------------------------------------
    // shm 扩展接口（非接口方法）
    // ------------------------------------------------------------------

    void SetCodec(comm::codec::CodecPtr codec) { codec_ = std::move(codec); }
    void SetConnType(comm::TcpConnType type) { conntype_ = type; }
    void SetID(uint64_t id) { id_ = id; }

    void SetConnectionCallback(comm::ConnectionCallback cb) { connectioncb_ = std::move(cb); }
    void SetMessageCallback(comm::AnyMessageCallback cb) { messagecb_ = std::move(cb); }
    void SetCloseCallback(comm::CloseCallback cb) { closecb_ = std::move(cb); }
    void SetVerifyCallback(comm::VerifyCallback cb) { verifycb_ = std::move(cb); }

    std::any* GetMutableContext() { return &context_; }
    std::any* GetMutableRpcContext() { return &rpc_context_; }

    /// @brief 连接 socket fd（owner 用于注册 POLLIN 断连检测）
    int SocketFd() const noexcept { return socket_fd_.Get(); }
    /// @brief 对端写入通知的 eventfd（owner 用于注册 POLLIN 实时读）
    int NotifyFd() const noexcept { return channel_.NotifyReadFd(); }
    /// @brief 访问底层通道（批量写入等高级用法）
    DefaultRingChannel& Channel() noexcept { return channel_; }

    // ------------------------------------------------------------------
    // owner 驱动接口（ShmServer / ShmAsyncClient 调用，应用层勿直接调用）
    // ------------------------------------------------------------------

    /**
     * @brief 握手与 fd 注册完成后调用（对应框架 OnConnection）
     *
     * 设置了 VerifyCallback 时先进入 verifing 状态并触发 verify 回调，
     * 否则直接进入 established。随后触发 connection 回调。
     */
    void OnConnection()
    {
        if (verifycb_)
        {
            state_ = comm::TcpConnStates::verifing;
            verifycb_(shared_from_this());
        }
        else
        {
            state_ = comm::TcpConnStates::established;
        }
        if (connectioncb_)
        {
            connectioncb_(shared_from_this());
        }
    }

    /**
     * @brief 消息分发入口（对应框架 OnMessage）
     */
    void OnMessage(const std::any& pkg)
    {
        if (messagecb_ && (state_ == comm::TcpConnStates::established || state_ == comm::TcpConnStates::verifing))
        {
            messagecb_(shared_from_this(), pkg);
        }
    }

    /**
     * @brief 验证结果（对应框架 OnVerify）：成功进入 established，失败关闭
     */
    void OnVerify(bool succ)
    {
        RemoveVerifyTimer();
        if (succ)
        {
            state_ = comm::TcpConnStates::established;
        }
        else
        {
            Close();
        }
    }

    /**
     * @brief 连接移除时调用（对应框架 OnClose，幂等）
     */
    void OnClose()
    {
        if (state_ != comm::TcpConnStates::closeed)
        {
            state_ = comm::TcpConnStates::closeed;
            if (closecb_)
            {
                closecb_(shared_from_this());
            }
        }
        if (context_.has_value())
        {
            context_.reset();
        }
        if (rpc_context_.has_value())
        {
            rpc_context_.reset();
        }
        RemoveVerifyTimer();
    }

    /**
     * @brief 消费读环中的数据，逐包调用 codec->Decode 并分发
     *
     * 数据跨环尾回绕时自动线性化到内部缓冲（少见路径）。
     * 数据不完整（codec 返回 0）时保留在环中等待下次通知——与 TCP 流语义一致。
     *
     * 读空后调用 channel_.PrepareWait 声明"即将睡眠"（按需通知）：写方此后
     * 才会再写 eventfd。PrepareWait 发现新数据到达时继续读，不会丢唤醒。
     *
     * @return 分发的消息条数；-1 协议错误（codec 要求断开），连接应移除
     */
    int DrainRing()
    {
        int total = 0;
        for (;;)
        {
            // 基线必须在读取之前取：此后到达的数据由 PrepareWait 复查发现
            const uint64_t seen = channel_.WritePos();
            const int n         = DrainRingOnce();
            if (n < 0)
            {
                return -1;
            }
            total += n;
            // 消息回调内关闭了连接：由 owner 收尾，无需再武装等待
            if (state_ == comm::TcpConnStates::closeing || state_ == comm::TcpConnStates::closeed)
            {
                break;
            }
            if (channel_.PrepareWait(seen))
            {
                break;
            }
        }
        return total;
    }

 private:
    /// @brief 单轮消费：读到环空或遇到不完整帧为止
    int DrainRingOnce()
    {
        if (!codec_)
        {
            return -1;
        }
        int count = 0;
        // 跨消息复用同一个 std::any：同类型消息解码时原地覆盖，避免
        // 每条消息重新分配（大对象收益明显）。回调只在本次调用期间有效。
        std::any pkg;

        for (;;)
        {
            const void* seg1  = nullptr;
            const void* seg2  = nullptr;
            uint32_t seg1_len = 0;
            uint32_t seg2_len = 0;
            if (channel_.Peek(&seg1, &seg1_len, &seg2, &seg2_len) != 0)
            {
                break;  // 环空
            }

            const comm::byte* buf = static_cast<const comm::byte*>(seg1);
            uint32_t buflen       = seg1_len;
            bool partial_view     = false;

            if (seg2_len > 0)
            {
                // 数据跨环尾：只线性化"当前帧"而不是全部可读数据。
                // 先探测首帧大小；codec 未报告时退化为线性化全部。
                const uint32_t total = seg1_len + seg2_len;
                uint32_t want        = total;

                size_t need      = 0;
                const int32_t rc = codec_->Decode(static_cast<const comm::byte*>(seg1), seg1_len, pkg, &need);
                if (rc == comm::kCodecDisconnect)
                {
                    return -1;
                }
                if (rc > 0)
                {
                    want = static_cast<uint32_t>(rc);  // 首帧完全在 seg1
                }
                else if (rc == comm::kCodecSkipPackage && need > 0 && need <= seg1_len)
                {
                    want = static_cast<uint32_t>(need);  // 心跳消费长度
                }
                else if (rc == comm::kCodecNeedMoreBuffer && need > seg1_len && need <= total)
                {
                    want = static_cast<uint32_t>(need);  // 首帧跨尾，按需复制
                }
                // 其他情况（codec 未报告 need 等）退化为 total

                if (want > total)
                {
                    want = total;
                }

                // 拷贝 want 字节；若 codec 需要更多（例如 seg1 连 tag 都不完整），
                // 按它报告的 need 再增长，最多 3 次，避免死循环
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    try
                    {
                        if (staging_.size() < want)
                        {
                            staging_.resize(want);
                        }
                    }
                    catch (const std::bad_alloc&)
                    {
                        return -1;  // 线性化缓冲分配失败
                    }
                    // want 可能小于 seg1_len（只复制首帧），必须按
                    // min(seg1_len, want) 截断，否则会越界写 staging_
                    const uint32_t first = (seg1_len < want) ? seg1_len : want;
                    std::memcpy(staging_.data(), seg1, first);
                    if (want > first)
                    {
                        std::memcpy(staging_.data() + first, seg2, want - first);
                    }

                    size_t more = 0;
                    const int32_t probe2 =
                        codec_->Decode(reinterpret_cast<const comm::byte*>(staging_.data()), want, pkg, &more);
                    if (probe2 == comm::kCodecNeedMoreBuffer && more > want && more <= total)
                    {
                        want = static_cast<uint32_t>(more);
                        continue;
                    }
                    break;
                }

                buf          = reinterpret_cast<const comm::byte*>(staging_.data());
                buflen       = want;
                partial_view = (want < total);
            }

            // 在连续视图上批量解码：整个批次只 CommitRead 一次
            uint32_t off = 0;
            for (;;)
            {
                size_t out        = 0;
                const int32_t len = codec_->Decode(buf + off, buflen - off, pkg, &out);
                if (len == comm::kCodecDisconnect)
                {
                    if (off)
                    {
                        channel_.CommitRead(off);
                    }
                    return -1;
                }
                if (len == comm::kCodecNeedMoreBuffer)
                {
                    break;  // 数据不完整，等待对端续写
                }
                if (len == comm::kCodecSkipPackage)
                {
                    // 解码方向的 -2：心跳包，out 为消费字节数
                    if (out == 0 || out > buflen - off)
                    {
                        if (off)
                        {
                            channel_.CommitRead(off);
                        }
                        return -1;  // 防御：非法消耗长度
                    }
                    off += static_cast<uint32_t>(out);
                    if (keepalive_)
                    {
                        SendKeepaliveResponse();
                    }
                    if (off == buflen)
                    {
                        break;
                    }
                    continue;
                }
                if (len < 0)
                {
                    if (off)
                    {
                        channel_.CommitRead(off);
                    }
                    return -1;  // 未知负值同样视为协议错误
                }
                // len > 0：成功解码一包
                if (static_cast<uint32_t>(len) > buflen - off)
                {
                    if (off)
                    {
                        channel_.CommitRead(off);
                    }
                    return -1;  // 防御：codec 消耗长度越界
                }
                off += static_cast<uint32_t>(len);
                OnMessage(pkg);
                ++count;
                if (off == buflen)
                {
                    break;
                }
            }

            if (off > 0)
            {
                channel_.CommitRead(off);
            }

            if (off == 0)
            {
                break;  // 没有进展：等待更多数据/通知
            }

            if (partial_view || off < buflen)
            {
                continue;  // 还有数据（或未完整帧）跨尾，重新 Peek
            }
            // off == buflen 且整段已消费：继续 Peek 下一批
        }
        return count;
    }

    /// @brief 启用"直接写 ring"的最小编码长度（小消息留在 encode_buf_）
    static constexpr uint32_t kDirectEncodeMinBytes = 256;

    /// @brief 编码缓冲按需扩容，超过上限返回 false
    bool GrowEncodeBuffer(size_t need)
    {
        if (need > max_buffer_size_)
        {
            return false;
        }
        if (need > encode_buf_.size())
        {
            encode_buf_.resize(need);
        }
        return true;
    }

    /// @brief 原始字节写入环（可跨多次 TryWrite），写完统一通知对端
    bool WriteRaw(const void* data, uint32_t len)
    {
        if (len == 0)
        {
            return true;
        }
        if (channel_.WritableBytes() < len)
        {
            return false;  // 环满（对端消费过慢），整体失败不半截写入
        }

        auto* p       = static_cast<const char*>(data);
        uint32_t left = len;
        while (left > 0)
        {
            // 单次 TryWrite 上限为 kMaxWriteSize（Capacity/2）
            const uint32_t chunk = std::min(left, static_cast<uint32_t>(DefaultRingChannel::kMaxWriteSize));
            if (channel_.TryWrite(p, chunk) != 0)
            {
                return false;  // 理论不可达（已预检空间）
            }
            p += chunk;
            left -= chunk;
        }
        channel_.NotifyPeer();
        return true;
    }

    /// @brief 收到心跳包后回发心跳响应
    void SendKeepaliveResponse()
    {
        if (!codec_)
        {
            return;
        }
        try
        {
            for (;;)
            {
                size_t size = 0;
                const int32_t len =
                    codec_->SendKeepAlive(reinterpret_cast<comm::byte*>(encode_buf_.data()), encode_buf_.size(), &size);
                if (len > 0)
                {
                    WriteRaw(encode_buf_.data(), static_cast<uint32_t>(len));
                    return;
                }
                if (len != comm::kCodecNeedMoreBuffer)
                {
                    return;  // -1/-2：无法生成响应，放弃
                }
                // 缓冲不足：扩容后重试。所需大小为 0（内部错误）或不超过现有
                // 缓冲仍要求扩容（codec 违约）时放弃——绝不能把 encode_buf_ 中
                // 上一条消息的残留字节当作响应发给对端
                if (size == 0 || size <= encode_buf_.size() || !GrowEncodeBuffer(size))
                {
                    return;
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            // 分配失败：放弃本次心跳响应（不影响连接）
        }
    }

    void RemoveVerifyTimer()
    {
        if (verify_timer_)
        {
            loop_->RemoveTimer(static_cast<int>(verify_timer_));
            verify_timer_ = 0;
        }
    }

    EventLoop* loop_;                                         ///< 事件循环（验证定时器用）
    uint64_t id_;                                             ///< 连接 ID
    int state_ = comm::TcpConnStates::closeed;                ///< 连接状态
    UniqueFd socket_fd_;                                      ///< 连接 socket（断连检测）
    DefaultRingChannel channel_;                              ///< 双向共享内存通道
    comm::codec::CodecPtr codec_;                             ///< 编解码器（每连接一个实例）
    comm::ConnectionCallback connectioncb_;                   ///< 连接建立回调
    comm::AnyMessageCallback messagecb_;                      ///< 消息回调
    comm::CloseCallback closecb_;                             ///< 连接关闭回调
    comm::VerifyCallback verifycb_;                           ///< 验证回调
    std::any context_;                                        ///< 用户上下文
    std::any rpc_context_;                                    ///< RPC 上下文
    comm::TimerID verify_timer_ = 0;                          ///< 验证超时定时器
    comm::CloseType close_type_{comm::CloseType::Passive};    ///< 关闭类型
    comm::TcpConnType conntype_{comm::TcpConnType::Unknown};  ///< 连接类型
    bool keepalive_{false};                                   ///< 是否自动响应心跳包
    std::vector<char> encode_buf_;                            ///< 编码缓冲区
    std::vector<char> staging_;                               ///< 跨环尾解码线性化缓冲（惰性分配）
    size_t max_buffer_size_ = 1024 * 1024;                    ///< 编码缓冲上限
};

using ShmConnPtr  = std::shared_ptr<ShmConnection>;
using ShmConnWptr = std::weak_ptr<ShmConnection>;

}  // namespace shm

#endif  // SHM_IPC_SHM_CONNECTION_HPP_
