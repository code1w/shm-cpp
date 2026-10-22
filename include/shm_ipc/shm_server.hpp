/**
 * @file shm_server.hpp
 * @brief 共享内存服务端实现（C++17）：shm::ShmServer 实现 comm::ITcpServer
 *
 * 应用层只依赖 comm::ITcpServer 接口编程，传输实现可替换：
 * @code
 * shm::EventLoop loop;
 * shm::ShmServer server(&loop, "shm:///tmp/app.socket", "app_server");
 * server.SetCodecFactory([] { return comm::codec::Adapt(std::make_shared<MyCodec>()); });
 * server.SetConnectionCallback([](const comm::TcpConnPtr &conn) { ... });
 * server.SetCloseCallback([](const comm::TcpConnPtr &conn) { ... });
 * server.SetMessageCallback([](const comm::TcpConnPtr &conn, const std::any &pkg) { ... });
 * server.Start();
 * loop.Run();
 * @endcode
 *
 * shm 实现的特点：
 * - uri 支持 "shm:///path"、"uds:///path" 或裸路径（本地 Unix socket）
 * - accept 后的 fd 交换握手由内部后台线程完成（每连接阻塞超时 5 秒），
 *   恶意/僵死连接只占用握手线程，不会拖慢事件循环上的其他连接；
 *   握手完成后经 eventfd 通知事件循环收编（回调仍在 loop 线程触发）
 * - SetRateLimit / SetKeepAlive 仅记录（本地通信无需限流，断连由 EOF 即时感知）
 * - 所有方法必须在 EventLoop 所在线程调用；进程需自行忽略 SIGPIPE
 */

#ifndef SHM_IPC_SHM_SERVER_HPP_
#define SHM_IPC_SHM_SERVER_HPP_

#include <assert.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <comm/server.h>

#include "common.hpp"
#include "event_loop.hpp"
#include "shm_connection.hpp"

namespace shm {

/**
 * @brief 共享内存服务端（实现 comm::ITcpServer，对应框架 io::TcpServer）
 *
 * 单线程模型：外部持有一个 EventLoop，ShmServer 在其上注册
 * listen fd、各连接的 eventfd（实时读）与 socket fd（断连检测）。
 * fd 交换握手由内部握手线程完成（见文件头注释），不影响事件循环。
 */
class ShmServer : public comm::ITcpServer
{
 public:
    /**
     * @param loop 外部事件循环（必须比 ShmServer 存活更久）
     * @param uri  监听地址："shm:///path"、"uds:///path" 或裸 Unix socket 路径
     * @param name 服务端名称（日志用）
     */
    ShmServer(EventLoop *loop, std::string_view uri,
              std::string_view name = "shm_server")
        : loop_(loop), uri_(std::string(uri)), name_(std::string(name))
    {
    }

    ~ShmServer() override { Stop(); }

    // 禁止复制和移动（EventLoop 回调持有 this）
    ShmServer(const ShmServer &)            = delete;
    ShmServer &operator=(const ShmServer &) = delete;
    ShmServer(ShmServer &&)                 = delete;
    ShmServer &operator=(ShmServer &&)      = delete;

    // ------------------------------------------------------------------
    // comm::ITcpServer 接口实现
    // ------------------------------------------------------------------

    void SetVerifyCallback(comm::VerifyCallback cb) override { verifycb_ = std::move(cb); }
    void SetConnectionCallback(comm::ConnectionCallback cb) override { connectioncb_ = std::move(cb); }
    void SetCloseCallback(comm::CloseCallback cb) override { closecb_ = std::move(cb); }
    void SetMessageCallback(comm::AnyMessageCallback cb) override { messagecb_ = std::move(cb); }
    void SetCodecFactory(comm::codec::CodecFactoryCallback cb) override { codec_factory_ = std::move(cb); }
    void SetStartFailedCallback(comm::StartFailedCallback cb) override { failedcb_ = std::move(cb); }
    void SetStartSuccessCallback(comm::StartSuccessCallback cb) override { successcb_ = std::move(cb); }

    /**
     * @brief 心跳开关（shm 断连由 socket EOF 即时感知，此开关仅控制
     *        收到心跳包时是否自动回发心跳响应）
     */
    void SetKeepAlive(bool keepalive, uint32_t io_keepalive_timeout) override
    {
        keepalive_            = keepalive;
        io_keepalive_timeout_ = io_keepalive_timeout;
    }

    void ResizeBuffer(size_t size, size_t max) override
    {
        init_buffer_size_ = size;
        max_buffer_size_  = max;
    }

    /// @brief 限流（shm 本地通信无需限流，仅记录保持接口兼容）
    void SetRateLimit(uint32_t qps) override { rate_limit_ = qps; }

    /**
     * @brief 开始监听（同步完成 bind/listen 并启动握手线程，
     *        accept 由事件循环驱动）
     * @return true 成功；失败触发 failed 回调并返回 false
     */
    bool Start() noexcept override
    {
        assert(connectioncb_);
        assert(messagecb_);
        assert(closecb_);
        assert(codec_factory_);
        return Listen();
    }

    /**
     * @brief 停止服务：唤醒并等待握手线程退出、移除监听、关闭全部连接、
     *        unlink socket 路径（幂等）
     */
    void Stop() override
    {
        if (!started_)
            return;
        started_ = false;

        // 1) 通知握手线程退出，并 shutdown 未完成握手的 socket
        //    （打断其阻塞中的 recvmsg，使其立即失败返回）
        {
            std::lock_guard<std::mutex> lk(hs_mtx_);
            hs_stopping_ = true;
            for (auto &fd : hs_pending_)
                if (fd.Get() >= 0)
                    ::shutdown(fd.Get(), SHUT_RDWR);
            if (hs_current_.Get() >= 0)
                ::shutdown(hs_current_.Get(), SHUT_RDWR);
        }
        hs_cv_.notify_all();
        if (hs_thread_.joinable())
            hs_thread_.join();

        // 2) 注销监听与握手完成通知 fd
        if (listen_fd_)
        {
            loop_->RemoveFd(listen_fd_.Get());
            listen_fd_.Reset();
        }
        if (hs_wake_fd_)
            loop_->RemoveFd(hs_wake_fd_.Get());
        // hs_wake_fd_ 不在此关闭：握手线程已 join，留待成员析构，
        // 避免关闭后 fd 号被复用导致握手线程误写

        // 3) 丢弃握手完成但未及收编的连接（未触发过 OnConnection，静默销毁）
        {
            std::lock_guard<std::mutex> lk(hs_mtx_);
            hs_pending_.clear();
            hs_done_.clear();
            hs_current_.Reset();
        }

        // RemoveConn 会修改 conns_，先拷贝一份再逐个移除
        std::vector<ShmConnPtr> conns;
        conns.reserve(conns_.size());
        for (auto &[id, conn] : conns_)
            conns.push_back(conn);
        for (auto &conn : conns)
            RemoveConn(conn);
        ::unlink(UriToPath(uri_).c_str());
    }

    // ------------------------------------------------------------------
    // shm 扩展接口（非接口方法）
    // ------------------------------------------------------------------

    /**
     * @brief 最大并发客户端数（含握手中的连接，超出后直接拒绝，默认 16）
     */
    void SetMaxClients(int n) { max_clients_ = n; }

    /**
     * @brief 向指定连接发送消息（扩展接口；接口等价写法为 FindConn(id)->SendCmd(msg)）
     * @return true 成功，false 连接不存在或发送失败
     */
    bool SendTo(uint64_t conn_id, const std::any &pkg)
    {
        auto conn = FindConn(conn_id);
        if (!conn)
            return false;
        return conn->SendCmd(pkg);
    }

    /**
     * @brief 向所有连接广播消息（单条失败不影响其他连接）
     */
    void Broadcast(const std::any &pkg)
    {
        for (auto &[id, conn] : conns_)
            conn->SendCmd(pkg);
    }

    /** @brief 主动关闭指定连接（立即移除并触发 close 回调） */
    void CloseConn(uint64_t conn_id)
    {
        auto conn = FindConn(conn_id);
        if (conn)
        {
            conn->Close();
            RemoveConn(conn);
        }
    }

    /** @brief 按 ID 查找连接 */
    ShmConnPtr FindConn(uint64_t conn_id) const
    {
        auto it = conns_.find(conn_id);
        return it != conns_.end() ? it->second : nullptr;
    }

    std::size_t ConnCount() const { return conns_.size(); }
    const std::string &Name() const { return name_; }
    const std::string &Uri() const { return uri_; }
    void ReName(const std::string &name) { name_ = name; }

 private:
    /// @brief 创建监听、握手线程与完成通知 fd，并注册 accept 回调
    bool Listen()
    {
        // 已在监听：直接成功。不做此守卫时，二次 Start 会对仍 joinable 的
        // hs_thread_ move 赋值（std::terminate），且 hs_wake_fd_ 赋值会
        // 关闭旧 eventfd（旧线程仍在使用）
        if (started_)
            return true;

        try
        {
            listen_fd_ = ListenUnixSocket(UriToPath(uri_).c_str());
            {
                // 支持 Stop 后再次 Start：复位握手线程状态
                std::lock_guard<std::mutex> lk(hs_mtx_);
                hs_stopping_ = false;
            }
            hs_wake_fd_ = CreateEventfd();
            hs_thread_  = std::thread([this] { HandshakeWorker(); });
        }
        catch (const std::exception &e)
        {
            listen_fd_.Reset();
            if (hs_thread_.joinable())
                hs_thread_.join();
            if (failedcb_)
                failedcb_(std::string("listen failed, uri: ") + uri_ + ", " + e.what());
            return false;
        }
        started_ = true;
        loop_->AddFd(listen_fd_.Get(),
                     [this](int fd, short revents) { OnAccept(fd, revents); });
        loop_->AddFd(hs_wake_fd_.Get(),
                     [this](int fd, short revents) { OnHandshakeComplete(fd, revents); });
        if (successcb_)
            successcb_();
        return true;
    }

    /// @brief accept 回调：接受新连接，fd 交给握手线程（不阻塞事件循环）
    void OnAccept(int lfd, short /*revents*/)
    {
        {
            // 上限计数含握手中的连接，防止握手期间超额
            std::lock_guard<std::mutex> lk(hs_mtx_);
            std::size_t inflight = hs_pending_.size() + hs_done_.size()
                                 + (hs_current_.Get() >= 0 ? 1 : 0);
            if (max_clients_ <= 0 ||
                conns_.size() + inflight >= static_cast<std::size_t>(max_clients_))
            {
                UniqueFd rejected{::accept(lfd, nullptr, nullptr)};
                return;  // accept 后立即关闭，对端握手时收到 EOF
            }
        }

        UniqueFd cfd{::accept(lfd, nullptr, nullptr)};
        if (!cfd)
            return;

        {
            std::lock_guard<std::mutex> lk(hs_mtx_);
            hs_pending_.push_back(std::move(cfd));
        }
        hs_cv_.notify_one();
    }

    /// @brief 握手线程主体：逐个完成 fd 交换（阻塞，每连接 5 秒超时）
    void HandshakeWorker()
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk(hs_mtx_);
                hs_cv_.wait(lk, [this] { return hs_stopping_ || !hs_pending_.empty(); });
                if (hs_pending_.empty())
                    return;  // stopping 且无待握手连接
                hs_current_ = std::move(hs_pending_.front());
                hs_pending_.pop_front();
            }

            DefaultRingChannel channel;
            bool ok = false;
            try
            {
                channel = DefaultRingChannel::CreateRingChannel(hs_current_.Get(),
                                                                ChannelRole::kServer);
                ok      = true;
            }
            catch (const std::exception &)
            {
                // 握手失败（对端超时、乱发数据或 Stop 的 shutdown 打断）：
                // fd 与半成品通道由 RAII 释放
            }

            bool notify = false;
            {
                std::lock_guard<std::mutex> lk(hs_mtx_);
                if (ok && !hs_stopping_)
                {
                    hs_done_.push_back(
                        HandshakeDone{std::move(hs_current_), std::move(channel)});
                    notify = true;
                }
                hs_current_.Reset();
            }
            if (notify)
            {
                uint64_t v = 1;
                // hs_wake_fd_ 与 server 同生命周期（Stop 不关闭），写不会命中复用 fd
                (void)::write(hs_wake_fd_.Get(), &v, sizeof(v));
            }
        }
    }

    /// @brief 握手完成回调（事件循环线程）：收编新连接并触发 OnConnection
    void OnHandshakeComplete(int efd, short /*revents*/)
    {
        DefaultRingChannel::DrainNotify(efd);

        std::deque<HandshakeDone> ready;
        {
            std::lock_guard<std::mutex> lk(hs_mtx_);
            ready.swap(hs_done_);
        }
        while (!ready.empty())
        {
            HandshakeDone done = std::move(ready.front());
            ready.pop_front();
            if (!started_)
                break;  // 回调内已调用 Stop()：剩余连接静默丢弃
            FinishAccept(std::move(done.sockfd), std::move(done.channel));
        }
    }

    /// @brief 握手成功后的连接建立（原 OnAccept 后半段，事件循环线程）
    void FinishAccept(UniqueFd cfd, DefaultRingChannel channel)
    {
        if (static_cast<int>(conns_.size()) >= max_clients_)
            return;  // 握手期间已达上限：静默丢弃（RAII 关闭）

        auto codec = codec_factory_ ? codec_factory_() : nullptr;
        if (!codec)
            return;  // codec 工厂返回空（同框架：拒绝该连接）

        uint64_t id = next_id_++;
        auto conn   = std::make_shared<ShmConnection>(loop_, id, std::move(cfd),
                                                      std::move(channel));
        codec->keepalive = keepalive_;
        codec->conn_id   = id;
        conn->SetMessageCallback(messagecb_);
        conn->SetCloseCallback(closecb_);
        conn->SetConnectionCallback(connectioncb_);
        conn->SetVerifyCallback(verifycb_);
        conn->SetCodec(codec);
        conn->SetKeepAlive(keepalive_);
        conn->SetConnType(comm::TcpConnType::TcpConn_Task);
        conn->ResizeBuffer(init_buffer_size_, max_buffer_size_);

        conns_[id] = conn;

        // eventfd：对端写入新数据，实时读取
        loop_->AddFd(conn->NotifyFd(),
                     [this, id](int fd, short revents) { OnConnNotify(fd, revents, id); });
        // socket：仅用于断连检测
        loop_->AddFd(conn->SocketFd(),
                     [this, id](int fd, short revents) { OnConnSocket(fd, revents, id); });

        conn->OnConnection();
    }

    /// @brief eventfd 回调：对端写入新数据，立即消费并解码分发
    void OnConnNotify(int efd, short /*revents*/, uint64_t conn_id)
    {
        DefaultRingChannel::DrainNotify(efd);
        // 持有本地拷贝：消息回调内可能调用 Close()/CloseConn() 移除连接
        auto conn = FindConn(conn_id);
        if (!conn)
            return;
        int n = conn->DrainRing();
        // 协议错误，或消息回调内调用了 Close()（closeing），收尾移除
        if (n < 0 || conn->State() == comm::TcpConnStates::closeing)
            RemoveConn(conn);
    }

    /// @brief socket 回调：断连检测（socket 在握手后不再承载数据）
    void OnConnSocket(int fd, short revents, uint64_t conn_id)
    {
        auto conn = FindConn(conn_id);
        if (!conn)
            return;
        if (revents & (POLLHUP | POLLERR | POLLIN))
        {
            char buf{};
            ssize_t n = ::read(fd, &buf, 1);
            if (n <= 0)
            {
                // 对端关闭：先读掉环中可能残留的最后数据，再移除
                conn->DrainRing();
                RemoveConn(conn);
            }
            // n > 0：握手后 socket 不应再有数据，忽略
        }
    }

    /// @brief 移除连接：注销 fd、移出连接表、触发 close 回调
    void RemoveConn(const ShmConnPtr &conn)
    {
        loop_->RemoveFd(conn->NotifyFd());
        loop_->RemoveFd(conn->SocketFd());
        conns_.erase(conn->Id());
        conn->OnClose();
    }

    /// @brief 握手完成待收编的连接（worker → 事件循环线程）
    struct HandshakeDone
    {
        UniqueFd sockfd;               ///< 握手完成的连接 socket
        DefaultRingChannel channel;    ///< 握手完成的双向通道
    };

    EventLoop *loop_;                                 ///< 外部事件循环
    std::string uri_;                                 ///< 监听 uri
    std::string name_;                                ///< 服务端名称
    UniqueFd listen_fd_;                              ///< 监听 socket
    std::unordered_map<uint64_t, ShmConnPtr> conns_;  ///< 连接表（id -> conn）
    uint64_t next_id_ = 1;                            ///< 下一个连接 ID
    int max_clients_ = 16;                            ///< 最大并发客户端数
    bool started_ = false;                            ///< 是否已 Start

    // ---- 异步握手（后台线程 ↔ 事件循环线程）----
    std::mutex hs_mtx_;                    ///< 保护下方队列与标志
    std::condition_variable hs_cv_;        ///< 唤醒握手线程
    std::deque<UniqueFd> hs_pending_;      ///< 待握手 socket（loop → worker）
    std::deque<HandshakeDone> hs_done_;    ///< 握手完成待收编（worker → loop）
    UniqueFd hs_current_;                  ///< worker 正在握手的 socket（Stop 时 shutdown 用）
    UniqueFd hs_wake_fd_;                  ///< 握手完成通知 eventfd（worker → loop）
    std::thread hs_thread_;                ///< 握手线程
    bool hs_stopping_ = false;             ///< 握手线程停止标志

    bool keepalive_{false};                           ///< 心跳开关（同框架字段）
    uint32_t io_keepalive_timeout_{0};                ///< 心跳超时（仅记录）
    uint32_t init_buffer_size_{128 * 1024};           ///< 编码缓冲初始大小（同框架默认）
    uint32_t max_buffer_size_{1024 * 1024};           ///< 编码缓冲上限（同框架默认）
    uint32_t rate_limit_{0};                          ///< 限流（仅记录）

    comm::ConnectionCallback connectioncb_;           ///< 连接建立回调
    comm::CloseCallback closecb_;                     ///< 连接关闭回调
    comm::AnyMessageCallback messagecb_;              ///< 消息回调
    comm::VerifyCallback verifycb_;                   ///< 验证回调
    comm::codec::CodecFactoryCallback codec_factory_; ///< codec 工厂
    comm::StartFailedCallback failedcb_;              ///< 启动失败回调
    comm::StartSuccessCallback successcb_;            ///< 启动成功回调
};

}  // namespace shm

#endif  // SHM_IPC_SHM_SERVER_HPP_
