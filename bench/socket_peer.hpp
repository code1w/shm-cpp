#pragma once
// Benchmark-only Unix stream adapter. Shares codec and EventLoop with SHM.
// Not a production TCP connection: verification/keepalive APIs are unsupported.
#include <comm/connection.h>
#include <shm_ipc/common.hpp>
#include <shm_ipc/event_loop.hpp>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace socket_bench {
inline bool coalesce=true;
inline int buffer_bytes=4*1024*1024;
inline void Check(bool condition) { if(!condition)throw std::runtime_error("socket benchmark invariant"); }
class Peer : public comm::ITcpConnection,public std::enable_shared_from_this<Peer> {
    shm::EventLoop* loop_;shm::UniqueFd fd_;uint64_t id_;comm::codec::CodecPtr codec_;
    comm::AnyMessageCallback message_;comm::CloseCallback close_;
    std::vector<comm::byte> tx_,rx_;size_t tx_size_=0,tx_off_=0,rx_size_=0;
    short mask_=POLLIN;shm::EventLoop::HookId hook_=0;bool open_=true,failed_=false;
    std::any context_,rpc_;
    uint64_t sends_=0,short_writes_=0,write_eagain_=0,receives_=0;
    void Watch(short mask) {
        if(mask==mask_)return;
        loop_->RemoveFd(fd_.Get());mask_=mask;Register();
    }
    void Register() {
        std::weak_ptr<Peer> weak=shared_from_this();
        loop_->AddFd(fd_.Get(),[weak](int,short events){if(auto p=weak.lock())p->Io(events);},mask_);
    }
    void Flush() {
        if(!open_||failed_)return;
        while(tx_off_<tx_size_) {
            ++sends_;
            ssize_t n=::send(fd_.Get(),tx_.data()+tx_off_,tx_size_-tx_off_,MSG_NOSIGNAL);
            if(n>0){if(size_t(n)<tx_size_-tx_off_)++short_writes_;tx_off_+=size_t(n);continue;}
            if(n<0&&errno==EINTR)continue;
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){++write_eagain_;Watch(POLLIN|POLLOUT);return;}
            // Do not erase the executing post-dispatch hook. Shutdown makes
            // the next I/O event perform Close outside the hook iteration.
            failed_=true;::shutdown(fd_.Get(),SHUT_RDWR);return;
        }
        tx_size_=tx_off_=0;Watch(POLLIN);
    }
    void Io(short events) {
        if(!open_)return;
        if(failed_){Close();return;}
        if(events&(POLLIN|POLLHUP|POLLERR)) {
            std::any pkg;bool eof=false;
            for(;;) {
                if(rx_size_==rx_.size()) {
                    if(rx_.size()>=8*1024*1024){failed_=true;break;}
                    rx_.resize(std::min<size_t>(8*1024*1024,rx_.size()*2));
                }
                ++receives_;
                ssize_t n=::recv(fd_.Get(),rx_.data()+rx_size_,rx_.size()-rx_size_,0);
                if(n>0)rx_size_+=size_t(n);
                else if(n<0&&errno==EINTR)continue;
                else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
                else {eof=true;break;}
                size_t consumed=0;
                while(consumed<rx_size_&&open_) {
                    size_t need=0;
                    int32_t len=codec_->Decode(rx_.data()+consumed,rx_size_-consumed,pkg,&need);
                    if(len==0)break;
                    if(len<0||size_t(len)>rx_size_-consumed){failed_=true;break;}
                    consumed+=size_t(len);message_(shared_from_this(),pkg);
                }
                if(consumed){rx_size_-=consumed;if(rx_size_)std::memmove(rx_.data(),rx_.data()+consumed,rx_size_);}
                if(open_)Flush(); // reply as soon as this receive chunk is decoded
                if(!open_||failed_)break;
            }
            if(open_&&(failed_||eof))Close();
        }
        if(open_&&(events&POLLOUT))Flush();
    }
public:
    Peer(shm::EventLoop* loop,shm::UniqueFd fd,uint64_t id,comm::codec::CodecPtr codec,
         comm::AnyMessageCallback message,comm::CloseCallback close)
      :loop_(loop),fd_(std::move(fd)),id_(id),codec_(std::move(codec)),message_(std::move(message)),close_(std::move(close)),tx_(128*1024),rx_(128*1024) {
        int flags=::fcntl(fd_.Get(),F_GETFL);Check(flags>=0&&::fcntl(fd_.Get(),F_SETFL,flags|O_NONBLOCK)==0);
        int request=buffer_bytes;
        Check(::setsockopt(fd_.Get(),SOL_SOCKET,SO_SNDBUF,&request,sizeof(request))==0);
        Check(::setsockopt(fd_.Get(),SOL_SOCKET,SO_RCVBUF,&request,sizeof(request))==0);
        int snd=0,rcv=0;socklen_t size=sizeof(int);
        Check(::getsockopt(fd_.Get(),SOL_SOCKET,SO_SNDBUF,&snd,&size)==0);
        Check(::getsockopt(fd_.Get(),SOL_SOCKET,SO_RCVBUF,&rcv,&size)==0);
        std::printf("{\"role\":\"socket_buffers\",\"send\":%d,\"receive\":%d,\"coalesce\":%s}\n",snd,rcv,coalesce?"true":"false");std::fflush(stdout);
    }
    ~Peer() override { if(hook_)loop_->RemovePostDispatchHook(hook_);loop_->RemoveFd(fd_.Get()); }
    void Start() {
        Register();std::weak_ptr<Peer> weak=shared_from_this();
        Check(loop_->AddPostDispatchHook([weak]{if(auto p=weak.lock())p->Flush();},&hook_)==shm::ErrorCode::kOk);
    }
    void InitialFlush(){Flush();}
    bool SendCmd(const std::any& pkg) override {
        if(!open_||failed_)return false;
        int32_t bytes=codec_->EncodedSize(pkg);if(bytes<=0||bytes>4*1024*1024)return false;
        // Retain partial writes; compact only when an actual pending write
        // would otherwise exhaust buffer space. No per-message allocations.
        if(tx_off_&&tx_size_+size_t(bytes)>tx_.size()){
            std::memmove(tx_.data(),tx_.data()+tx_off_,tx_size_-tx_off_);tx_size_-=tx_off_;tx_off_=0;
        }
        if(tx_size_+size_t(bytes)>8*1024*1024)return false;
        if(tx_size_+size_t(bytes)>tx_.size())tx_.resize(std::max(tx_.size()*2,tx_size_+size_t(bytes)));
        size_t need=0;int32_t len=codec_->Encode(pkg,tx_.data()+tx_size_,tx_.size()-tx_size_,&need);
        if(len!=bytes)return false;
        tx_size_+=size_t(len);
        if(!coalesce)Flush();
        return !failed_;
    }
    void Close() override {
        if(!open_)return;
        open_=false;::shutdown(fd_.Get(),SHUT_RDWR);
        loop_->RemoveFd(fd_.Get());if(hook_){loop_->RemovePostDispatchHook(hook_);hook_=0;}
        std::printf("{\"role\":\"socket_io\",\"send_calls\":%llu,\"short_writes\":%llu,\"write_eagain\":%llu,\"recv_calls\":%llu}\n",(unsigned long long)sends_,(unsigned long long)short_writes_,(unsigned long long)write_eagain_,(unsigned long long)receives_);std::fflush(stdout);
        auto self=shared_from_this();close_(self);
    }
    int32_t State() const override{return open_?comm::TcpConnStates::established:comm::TcpConnStates::closeed;}
    uint64_t Id() const override{return id_;}
    std::string RemoteAddr() const override{return "benchmark-uds";}
    comm::CloseType GetCloseType() const override{return comm::CloseType::Unknow;}
    void SetContext(std::any a) override{context_=std::move(a);}const std::any& Context()const override{return context_;}
    void SetRpcContext(std::any a) override{rpc_=std::move(a);}const std::any& RpcContext()const override{return rpc_;}
    void StartVerifyTimer(int64_t,int64_t,comm::TimeAction)override{throw std::logic_error("benchmark only");}
    void SetKeepAlive(bool)override{throw std::logic_error("benchmark only");}
    void ResizeBuffer(size_t,size_t)override{throw std::logic_error("benchmark only");}
    comm::codec::CodecPtr GetCodec()override{return codec_;}
};
class Server {
    shm::EventLoop* loop_;std::string path_;shm::UniqueFd listener_;
    std::unordered_map<uint64_t,std::shared_ptr<Peer>> peers_;uint64_t next_=1;
    comm::codec::CodecFactoryCallback factory_;comm::ConnectionCallback connected_;comm::AnyMessageCallback message_;comm::CloseCallback closed_;
public:
    Server(shm::EventLoop* l,std::string p,const char*):loop_(l),path_(std::move(p)){}
    ~Server(){Stop();}
    void SetCodecFactory(comm::codec::CodecFactoryCallback f){factory_=std::move(f);}
    void SetConnectionCallback(comm::ConnectionCallback f){connected_=std::move(f);}
    void SetMessageCallback(comm::AnyMessageCallback f){message_=std::move(f);}
    void SetCloseCallback(comm::CloseCallback f){closed_=std::move(f);}
    bool Start(){
        if(shm::ListenUnixSocket(path_,&listener_)!=shm::ErrorCode::kOk)return false;
        int flags=::fcntl(listener_.Get(),F_GETFL);Check(flags>=0&&::fcntl(listener_.Get(),F_SETFL,flags|O_NONBLOCK)==0);
        loop_->AddFd(listener_.Get(),[this](int fd,short){
            for(;;){int child=::accept4(fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
                if(child<0){if(errno==EINTR)continue;Check(errno==EAGAIN||errno==EWOULDBLOCK);break;}
                auto id=next_++;auto peer=std::make_shared<Peer>(loop_,shm::UniqueFd(child),id,factory_(),message_,[this](const comm::TcpConnPtr& c){peers_.erase(c->Id());closed_(c);});
                peers_[id]=peer;peer->Start();connected_(peer);
            }
        });return true;
    }
    void Stop(){
        if(!listener_)return;
        loop_->RemoveFd(listener_.Get());listener_.Reset();
        while(!peers_.empty()){auto p=peers_.begin()->second;p->Close();}
        ::unlink(path_.c_str());
    }
};
class Client {
    shm::EventLoop* loop_;std::shared_ptr<Peer> peer_;
    comm::codec::CodecFactoryCallback factory_;comm::ConnectionCallback connected_;comm::AnyMessageCallback message_;comm::CloseCallback closed_;comm::ConnectErrorCallback error_;
public:
    explicit Client(shm::EventLoop* l):loop_(l){}
    ~Client(){Close();}
    void SetCodecFactory(comm::codec::CodecFactoryCallback f){factory_=std::move(f);}
    void SetConnectionCallback(comm::ConnectionCallback f){connected_=std::move(f);}
    void SetMessageCallback(comm::AnyMessageCallback f){message_=std::move(f);}
    void SetCloseCallback(comm::CloseCallback f){closed_=std::move(f);}
    void SetConnectErrorCallback(comm::ConnectErrorCallback f){error_=std::move(f);}
    void Connect(const std::string& path){
        shm::UniqueFd fd;if(shm::ConnectUnixSocket(path,&fd)!=shm::ErrorCode::kOk){error_(false,path);return;}
        auto p=std::make_shared<Peer>(loop_,std::move(fd),1,factory_(),message_,[this](const comm::TcpConnPtr& c){peer_.reset();closed_(c);});
        peer_=p;p->Start();connected_(p);p->InitialFlush();
    }
    void Close(){auto p=peer_;if(p)p->Close();}
};
} // namespace socket_bench
