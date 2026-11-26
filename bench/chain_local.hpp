#pragma once
#include "chain_stream.hpp"
#ifdef SHM_CHAIN_PROTOBUF
#include "chain_protobuf.hpp"
#endif
#include <shm_ipc/shm_server.hpp>
#include <shm_ipc/shm_async_client.hpp>

namespace chain_bench {
class LocalLink
{
    shm::EventLoop* loop_     = nullptr;
    const char* path_         = nullptr;
    void* context_            = nullptr;
    PacketHandler packet_     = nullptr;
    CloseHandler closed_      = nullptr;
    ConnectHandler connected_ = nullptr;
#ifdef SHM_CHAIN_PROTOBUF
    ProtobufCodec codec_;
    ProtoOutgoing outgoing_;
    std::any receive_package_{static_cast<const Frame*>(nullptr)};
#else
    WireCodec codec_;  // outlives server/client connections, including aliasing CodecPtr
#endif
    std::unique_ptr<shm::ShmServer> server_;
    std::unique_ptr<shm::ShmAsyncClient> client_;
    comm::TcpConnPtr peer_;
#ifdef SHM_CHAIN_PROTOBUF
    std::any package_{static_cast<const ProtoOutgoing*>(&outgoing_)};
#else
    std::any package_{static_cast<const Frame*>(nullptr)};
#endif
    Stream stream_;
    shm::UniqueFd listener_;
    bool uds_ = false, path_owned_ = false;
    comm::codec::CodecPtr Codec() noexcept { return comm::codec::CodecPtr(comm::codec::CodecPtr{}, &codec_); }
    static shm::ErrorCode StreamPacket(void* self, const Frame& frame)
    {
        auto* p = static_cast<LocalLink*>(self);
#ifdef SHM_CHAIN_PROTOBUF
        const auto ec = p->codec_.DecodeFrame(frame, &p->receive_package_);
        if (ec != shm::ErrorCode::kOk)
            return ec;
        return p->packet_(p->context_, *ProtobufCodec::Get(p->receive_package_));
#else
        return p->packet_(p->context_, frame);
#endif
    }
    static void StreamClosed(void* self, shm::ErrorCode ec)
    {
        auto* p = static_cast<LocalLink*>(self);
        p->closed_(p->context_, ec);
    }
    void ShmMessage(const comm::TcpConnPtr& c, const std::any& message)
    {
        const auto* frame = codec_.Get(message);
        const auto ec     = frame ? packet_(context_, *frame) : shm::ErrorCode::kProtocol;
        if (ec != shm::ErrorCode::kOk)
        {
            c->Close();
            closed_(context_, ec);
        }
    }
    shm::ErrorCode ConfigureShmPeer(const comm::TcpConnPtr& c)
    {
#ifdef SHM_CHAIN_PROTOBUF
        auto* connection = dynamic_cast<shm::ShmConnection*>(c.get());
        if (!connection)
            return shm::ErrorCode::kProtocol;
        auto ec = connection->SetDirectEncodeMinimumBytes(0);
        if (ec != shm::ErrorCode::kOk)
            return ec;
        return codec_.CaptureSharedMaps();
#else
        (void)c;
        return shm::ErrorCode::kOk;
#endif
    }
#ifdef SHM_CHAIN_PROTOBUF
    shm::ErrorCode SendPrepared()
    {
        if (uds_)
            return stream_.SendEncoded(
                outgoing_.Bytes(),
                [](void* ctx, unsigned char* dst, uint32_t n) {
                    auto* link = static_cast<LocalLink*>(ctx);
                    return link->codec_.EncodePrepared(link->outgoing_, dst, n);
                },
                this);
        if (!peer_)
            return shm::ErrorCode::kPeerClosed;
        return peer_->SendCmd(package_) ? shm::ErrorCode::kOk : shm::ErrorCode::kTimeout;
    }
#endif
    void OnAccept()
    {
        shm::UniqueFd child{::accept4(listener_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
        if (!child)
        {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                closed_(context_, shm::ErrnoToErrorCode(errno));
            return;
        }
        if (stream_.Open())
        {
            closed_(context_, shm::ErrorCode::kProtocol);
            return;
        }
        const auto ec = stream_.Init(loop_, std::move(child), false, 0, this, StreamPacket, StreamClosed);
        if (ec != shm::ErrorCode::kOk)
        {
            closed_(context_, ec);
            return;
        }
        connected_(context_);
    }

 public:
    LocalLink()                            = default;
    LocalLink(const LocalLink&)            = delete;
    LocalLink& operator=(const LocalLink&) = delete;
    ~LocalLink() { Close(); }
    shm::ErrorCode Init(shm::EventLoop* loop, const char* path, bool server, bool uds, void* context,
                        PacketHandler packet, CloseHandler closed, ConnectHandler connected, bool endpoint = false)
    {
        loop_      = loop;
        path_      = path;
        uds_       = uds;
        context_   = context;
        packet_    = packet;
        closed_    = closed;
        connected_ = connected;
#ifdef SHM_CHAIN_PROTOBUF
        codec_.Configure(endpoint);
#else
        (void)endpoint;
#endif
        if (uds_)
        {
            if (server)
            {
                auto ec = shm::ListenUnixSocket(path, &listener_);
                if (ec != shm::ErrorCode::kOk)
                    return ec;
                path_owned_ = true;
                ec          = Nonblocking(listener_.Get());
                if (ec != shm::ErrorCode::kOk)
                    return ec;
                loop_->AddFd(listener_.Get(), [this](int, short) { OnAccept(); });
            }
            else
            {
                shm::UniqueFd fd;
                auto ec = shm::ConnectUnixSocket(path, &fd);
                if (ec != shm::ErrorCode::kOk)
                    return ec;
                ec = stream_.Init(loop_, std::move(fd), false, 0, this, StreamPacket, StreamClosed);
                if (ec != shm::ErrorCode::kOk)
                    return ec;
                connected_(context_);
                stream_.Flush();
            }
            return shm::ErrorCode::kOk;
        }
        if (server)
        {
            server_.reset(new (std::nothrow) shm::ShmServer(loop_, path, "chain"));
            if (!server_)
                return shm::ErrorCode::kNoMemory;
            server_->SetCodecFactory([this] { return Codec(); });
            server_->SetConnectionCallback([this](const comm::TcpConnPtr& c) {
                if (peer_)
                {
                    c->Close();
                    closed_(context_, shm::ErrorCode::kProtocol);
                    return;
                }
                const auto ec = ConfigureShmPeer(c);
                if (ec != shm::ErrorCode::kOk)
                {
                    c->Close();
                    closed_(context_, ec);
                    return;
                }
                peer_ = c;
                connected_(context_);
            });
            server_->SetMessageCallback([this](const comm::TcpConnPtr& c, const std::any& p) { ShmMessage(c, p); });
            server_->SetCloseCallback([this](const comm::TcpConnPtr& c) {
                if (peer_ == c)
                    peer_.reset();
                closed_(context_, shm::ErrorCode::kPeerClosed);
            });
            if (!server_->Start())
                return shm::ErrorCode::kSystem;
        }
        else
        {
            client_.reset(new (std::nothrow) shm::ShmAsyncClient(loop_));
            if (!client_)
                return shm::ErrorCode::kNoMemory;
            client_->SetCodecFactory([this] { return Codec(); });
            client_->SetConnectionCallback([this](const comm::TcpConnPtr& c) {
                const auto ec = ConfigureShmPeer(c);
                if (ec != shm::ErrorCode::kOk)
                {
                    c->Close();
                    closed_(context_, ec);
                    return;
                }
                peer_ = c;
                connected_(context_);
            });
            client_->SetMessageCallback([this](const comm::TcpConnPtr& c, const std::any& p) { ShmMessage(c, p); });
            client_->SetCloseCallback([this](const comm::TcpConnPtr&) {
                peer_.reset();
                closed_(context_, shm::ErrorCode::kPeerClosed);
            });
            client_->SetConnectErrorCallback(
                [this](bool, const std::string&) { closed_(context_, client_->LastError()); });
            client_->Connect(path);
            if (!peer_)
                return client_->LastError() == shm::ErrorCode::kOk ? shm::ErrorCode::kPeerClosed : client_->LastError();
        }
        return shm::ErrorCode::kOk;
    }
    shm::ErrorCode Send(const Frame& frame)
    {
#ifdef SHM_CHAIN_PROTOBUF
        outgoing_ = {&frame, nullptr, 0, 0};
        return SendPrepared();
#else

        if (uds_)
            return stream_.Send(frame);
        if (!peer_)
            return shm::ErrorCode::kPeerClosed;
        *std::any_cast<const Frame*>(&package_) = &frame;
        return peer_->SendCmd(package_) ? shm::ErrorCode::kOk : shm::ErrorCode::kTimeout;
#endif
    }
#ifdef SHM_CHAIN_PROTOBUF
    shm::ErrorCode SendProtobuf(const chain_bench_pb::Rpc& message, uint32_t magic)
    {
        const auto ec = codec_.Prepare(message, magic, &outgoing_);
        return ec == shm::ErrorCode::kOk ? SendPrepared() : ec;
    }
    const ProtoStats& EncodingStats() const noexcept { return codec_.Stats(); }
#endif
    void Close()
    {
        if (server_)
            server_->Stop();
        if (client_)
            client_->Close();
        stream_.Close();
        peer_.reset();
        if (listener_)
        {
            loop_->RemoveFd(listener_.Get());
            listener_.Reset();
        }
        if (path_owned_)
        {
            ::unlink(path_);
            path_owned_ = false;
        }
    }
};
}  // namespace chain_bench
