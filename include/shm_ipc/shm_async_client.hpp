/**
 * @file shm_async_client.hpp
 * @brief 共享内存异步客户端实现（C++17）：shm::ShmAsyncClient 实现 comm::ITcpAsyncClient
 *
 * 应用层只依赖 comm::ITcpAsyncClient 接口编程，传输实现可替换：
 * @code
 * shm::EventLoop loop;
 * shm::ShmAsyncClient client(&loop);
 * client.SetCodecFactory([] { return comm::codec::Adapt(std::make_shared<MyCodec>()); });
 * client.SetConnectionCallback([](const comm::TcpConnPtr &conn) { ... });
 * client.SetMessageCallback([](const comm::TcpConnPtr &conn, const std::any &pkg) { ... });
 * client.SetCloseCallback([](const comm::TcpConnPtr &conn) { ... });
 * client.SetConnectErrorCallback([](bool autoreconnect, const std::string &uri) { ... });
 * client.Connect("shm:///tmp/app.socket");
 * loop.Run();
 * @endcode
 *
 * shm 实现的特点：
 * - Connect() 是同步阻塞的（本地 connect + fd 交换握手，5 秒超时），
 *   但回调时机与框架一致：成功后触发 connection 回调，失败触发 error 回调
 * - uri 支持 "shm:///path"、"uds:///path" 或裸路径（本地 Unix socket）
 * - SetAutoReconnect 仅记录标志并透传给 error 回调（与框架一致，重连逻辑为 TODO）
 * - 所有方法必须在 EventLoop 所在线程调用；进程需自行忽略 SIGPIPE
 */

#ifndef SHM_IPC_SHM_ASYNC_CLIENT_HPP_
#define SHM_IPC_SHM_ASYNC_CLIENT_HPP_

#include <assert.h>
#include <unistd.h>

#include <any>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <comm/client.h>

#include "common.hpp"
#include "event_loop.hpp"
#include "shm_connection.hpp"

namespace shm {

/**
 * @brief 共享内存异步客户端（实现 comm::ITcpAsyncClient，对应框架 io::TcpAsyncClient）
 */
class ShmAsyncClient : public comm::ITcpAsyncClient
{
 public:
    /**
     * @param loop 外部事件循环（必须比 ShmAsyncClient 存活更久）
     */
    explicit ShmAsyncClient(EventLoop *loop) noexcept : loop_(loop) {}

    /// @brief 析构时静默移除 fd（不触发 close 回调，避免触碰已析构的用户对象）
    ~ShmAsyncClient() override
    {
        if (conn_ && loop_)
        {
            loop_->RemoveFd(conn_->NotifyFd());
            loop_->RemoveFd(conn_->SocketFd());
        }
    }

    // 禁止复制和移动（EventLoop 回调持有 this）
    ShmAsyncClient(const ShmAsyncClient &)            = delete;
    ShmAsyncClient &operator=(const ShmAsyncClient &) = delete;
    ShmAsyncClient(ShmAsyncClient &&)                 = delete;
    ShmAsyncClient &operator=(ShmAsyncClient &&)      = delete;

    // ------------------------------------------------------------------
    // comm::ITcpAsyncClient 接口实现
    // ------------------------------------------------------------------

    void SetVerifyCallback(comm::VerifyCallback cb) override { verifycb_ = std::move(cb); }
    void SetConnectionCallback(comm::ConnectionCallback cb) override { connectioncb_ = std::move(cb); }
    void SetMessageCallback(comm::AnyMessageCallback cb) override { messagecb_ = std::move(cb); }
    void SetCloseCallback(comm::CloseCallback cb) override { closecb_ = std::move(cb); }
    void SetConnectErrorCallback(comm::ConnectErrorCallback cb) override { errorcb_ = std::move(cb); }
    void SetCodecFactory(comm::codec::CodecFactoryCallback cb) override { codec_factory_ = std::move(cb); }

    void SetContext(std::any context) override { context_ = std::move(context); }
    const std::any &Context() const override { return context_; }
    void SetAutoReconnect(bool autoreconn) override { autoreconnect_ = autoreconn; }
    void ResizeBuffer(size_t size, size_t max) override
    {
        init_buffer_size_ = size;
        max_buffer_size_  = max;
    }

    /// @brief 客户端状态（取值见 comm::TcpClientStates）
    int32_t State() const override { return state_; }

    /// @brief 连接 ID（client 端固定为 1），未连接返回 0
    uint64_t Id() const override { return conn_ ? conn_->Id() : 0; }

    /**
     * @brief 连接服务端（同步 connect + 握手）
     *
     * uri 支持 "shm:///path"、"uds:///path" 或裸路径。
     * 成功后注册事件回调并触发 connection 回调；
     * 失败触发 error 回调（首参为 autoreconnect 标志，与框架一致）。
     */
    void Connect(const std::string_view &uri) override
    {
        assert(connectioncb_);
        assert(messagecb_);
        assert(closecb_);
        assert(codec_factory_);

        if (state_ == comm::TcpClientStates::connecting ||
            state_ == comm::TcpClientStates::established)
            return;

        state_ = comm::TcpClientStates::connecting;
        try
        {
            auto sock = ConnectUnixSocket(UriToPath(uri).c_str());
            auto channel = DefaultRingChannel::CreateRingChannel(sock.Get(),
                                                                 ChannelRole::kClient);

            auto codec = codec_factory_();
            if (!codec)
                throw std::runtime_error("shm client codec factory return null");

            uint64_t id = 1;
            auto conn   = std::make_shared<ShmConnection>(loop_, id, std::move(sock),
                                                          std::move(channel));
            codec->conn_id = id;
            conn->SetMessageCallback(messagecb_);
            conn->SetCloseCallback(closecb_);
            conn->SetConnectionCallback(connectioncb_);
            conn->SetVerifyCallback(verifycb_);
            conn->SetCodec(codec);
            conn->SetConnType(comm::TcpConnType::TcpConn_Client);
            conn->ResizeBuffer(init_buffer_size_, max_buffer_size_);
            if (context_.has_value())
                conn->SetContext(context_);
            conn_ = conn;

            loop_->AddFd(conn->NotifyFd(),
                         [this](int fd, short revents) { OnNotify(fd, revents); });
            loop_->AddFd(conn->SocketFd(),
                         [this](int fd, short revents) { OnSocket(fd, revents); });

            state_ = comm::TcpClientStates::established;
            conn->OnConnection();
        }
        catch (const std::exception &)
        {
            state_ = comm::TcpClientStates::close;
            if (errorcb_)
                errorcb_(autoreconnect_, std::string(uri));
            // TODO 自动重连（与框架一致，暂未实现）
        }
    }

    /**
     * @brief 发送消息包
     * @return true 成功；未连接或编码/写入失败返回 false
     */
    bool SendCmd(const std::any &pkg) override
    {
        auto conn = conn_;
        if (conn && state_ == comm::TcpClientStates::established)
            return conn->SendCmd(pkg);
        return false;
    }

    /**
     * @brief 主动关闭连接：shutdown 并触发 close 回调
     */
    void Close() override
    {
        if (state_ != comm::TcpClientStates::established)
            return;

        state_    = comm::TcpClientStates::closeing;
        auto conn = conn_;
        if (conn)
            conn->Close();
        Teardown();
    }

    // ------------------------------------------------------------------
    // shm 扩展接口（非接口方法）
    // ------------------------------------------------------------------

    /// @brief 当前连接对象（未连接返回 nullptr）
    ShmConnPtr Conn() const { return conn_; }

 private:
    /// @brief eventfd 回调：对端写入新数据，立即消费并解码分发
    void OnNotify(int efd, short /*revents*/)
    {
        DefaultRingChannel::DrainNotify(efd);
        // 持有本地拷贝：消息回调内可能调用 Close() 触发 Teardown()，
        // 若直接用 conn_ 访问会在 DrainRing 返回后踩空
        auto conn = conn_;
        if (!conn)
            return;
        int n = conn->DrainRing();
        // 协议错误，或消息回调内调用了 Close()（连接进入 closeing）
        if (n < 0 || conn->State() == comm::TcpConnStates::closeing)
            Teardown();
    }

    /// @brief socket 回调：断连检测（socket 在握手后不再承载数据）
    void OnSocket(int fd, short revents)
    {
        auto conn = conn_;  // 同 OnNotify，防止回调内 Teardown 后踩空
        if (!conn)
            return;
        if (revents & (POLLHUP | POLLERR | POLLIN))
        {
            char buf{};
            ssize_t n = ::read(fd, &buf, 1);
            if (n <= 0)
            {
                // 对端关闭：先读掉环中可能残留的最后数据，再收尾
                conn->DrainRing();
                Teardown();
            }
        }
    }

    /// @brief 收尾：移除 fd、触发 close 回调、释放连接（幂等）
    void Teardown()
    {
        if (state_ == comm::TcpClientStates::close)
            return;
        state_ = comm::TcpClientStates::close;
        // 先复位连接再触发回调：close 回调内允许立即重新 Connect()
        auto conn = std::move(conn_);
        if (conn)
        {
            loop_->RemoveFd(conn->NotifyFd());
            loop_->RemoveFd(conn->SocketFd());
            conn->OnClose();
        }
    }

    EventLoop *loop_ = nullptr;        ///< 外部事件循环
    ShmConnPtr conn_;                  ///< 当前连接（client 托管其生命周期）
    int state_ = comm::TcpClientStates::close;  ///< 连接状态（初值同框架：close）
    comm::ConnectionCallback connectioncb_;      ///< 连接建立回调
    comm::AnyMessageCallback messagecb_;         ///< 消息回调
    comm::CloseCallback closecb_;                ///< 连接关闭回调
    comm::VerifyCallback verifycb_;              ///< 验证回调
    comm::ConnectErrorCallback errorcb_;         ///< 连接失败回调
    comm::codec::CodecFactoryCallback codec_factory_;  ///< codec 工厂
    std::any context_;                 ///< 用户上下文（建连时同步到连接）
    bool autoreconnect_{false};        ///< 自动重连标志（透传给 error 回调）
    uint32_t init_buffer_size_{128 * 1024};    ///< 编码缓冲初始大小（同框架默认）
    uint32_t max_buffer_size_{1024 * 1024};    ///< 编码缓冲上限（同框架默认）
};

}  // namespace shm

#endif  // SHM_IPC_SHM_ASYNC_CLIENT_HPP_
