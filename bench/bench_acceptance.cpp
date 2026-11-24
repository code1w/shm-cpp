// Cross-process application acceptance harness: codec + EventLoop + connection.
// Sampled per-request RTT, fixed-size histogram, deterministic content checks.
#include <shm_ipc/shm_server.hpp>
#include "socket_peer.hpp"
#include <shm_ipc/shm_async_client.hpp>
#include <shm_ipc/simple_pod_codec.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

template<size_t N> struct AcceptancePod { static_assert(N>=16 && N%8==0); uint64_t seq; unsigned char data[N-8]; };
namespace shm { template<size_t N> struct TypeTag<AcceptancePod<N>> { static constexpr uint32_t value=0xAB000000u | static_cast<uint32_t>(N); }; }
using Small=AcceptancePod<16>;
using Big=AcceptancePod<4096>;
using Huge=AcceptancePod<1048576>;
SHM_IPC_REGISTER_POD(Small,0x41434331);
SHM_IPC_REGISTER_POD(Big,0x41434332);
SHM_IPC_REGISTER_POD(Huge,0x41434333);
static volatile sig_atomic_t stopping=0;
static void Signal(int) { stopping=1; }
static uint64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#define REQUIRE(x) do { if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(3); } } while(0)
static size_t Fds() { DIR* d=::opendir("/proc/self/fd");REQUIRE(d);size_t n=0;while(auto* e=::readdir(d))if(e->d_name[0]!='.')++n;::closedir(d);return n; }
static size_t Maps() { std::ifstream f("/proc/self/maps");std::string l;size_t n=0;while(std::getline(f,l))if(l.find("memfd:client_to_server")!=std::string::npos||l.find("memfd:server_to_client")!=std::string::npos)++n;return n; }
static bool Sample(uint64_t seq,int stride) {
    if(stride==1)return true;
    uint64_t x=seq+0x9e3779b97f4a7c15ULL;
    x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27))*0x94d049bb133111ebULL;
    return (x^(x>>31))%static_cast<uint64_t>(stride)==0;
}
struct Histogram {
    std::array<uint64_t,4096> bins{}; uint64_t samples=0,max=0;
    void Add(uint64_t n) {
        if(!n)n=1;
        unsigned e=63-__builtin_clzll(n);uint64_t base=uint64_t(1)<<e;
        unsigned sub=static_cast<unsigned>((n-base)*64/base);
        ++bins[e*64+sub];++samples;max=std::max(max,n);
    }
    uint64_t Percentile(double p) const {
        uint64_t rank=static_cast<uint64_t>(std::ceil(samples*p)); if(!rank)return 0;
        uint64_t sum=0;
        for(unsigned i=0;i<bins.size();++i)if((sum+=bins[i])>=rank) {
            unsigned e=i/64,sub=i%64;uint64_t base=uint64_t(1)<<e;
            return base+((sub+1)*base+63)/64; // bucket upper bound, <= ~1.56% error
        }
        return max;
    }
};
struct Options { std::string role,path,transport="shm";int bytes=16,batch=64,gap_us=0,sessions=1,stride=64,reconnect=0;double seconds=10,warmup=1;uint64_t messages=0;bool single_type=false; };
template<class T> std::any Make() { T p{};for(size_t i=0;i<sizeof(p.data);++i)p.data[i]=static_cast<unsigned char>(i*131+17);return p; }
template<class T> void SetSeq(std::any& a,uint64_t seq) { std::any_cast<T&>(a).seq=seq; }
template<class T> uint64_t Verify(const std::any& a,bool full) {
    const auto* p=std::any_cast<T>(&a);REQUIRE(p);
    if(full) { for(size_t i=0;i<sizeof(p->data);++i)REQUIRE(p->data[i]==static_cast<unsigned char>(i*131+17)); }
    else { REQUIRE(p->data[0]==17);REQUIRE(p->data[sizeof(p->data)-1]==static_cast<unsigned char>((sizeof(p->data)-1)*131+17)); }
    return p->seq;
}
struct PayloadOps {
    std::any (*make)();
    void (*set_seq)(std::any&,uint64_t);
    uint64_t (*verify)(const std::any&,bool);
    void (*register_type)();
};
template<class T> static void RegisterPayload() { shm::codec::SimplePodCodec::RegisterPod<T>(); }
template<size_t N> static PayloadOps Ops() {
    using T=AcceptancePod<N>;
    return {Make<T>,SetSeq<T>,Verify<T>,RegisterPayload<T>};
}
static PayloadOps SelectPayload(int bytes) {
    switch(bytes) {
        case 16:return Ops<16>();
        case 128:return Ops<128>();
        case 256:return Ops<256>();
        case 512:return Ops<512>();
        case 1024:return Ops<1024>();
        case 2048:return Ops<2048>();
        case 3072:return Ops<3072>();
        case 4096:return Ops<4096>();
        case 8192:return Ops<8192>();
        case 16384:return Ops<16384>();
        case 32768:return Ops<32768>();
        case 65536:return Ops<65536>();
        case 131072:return Ops<131072>();
        case 524288:return Ops<524288>();
        case 1048576:return Ops<1048576>();
        default:std::fprintf(stderr,"Unsupported POD message size: %d\n",bytes);std::exit(3);
    }
}
template<class Server,class Client>
static int Run(const Options& o) {
    const PayloadOps payload=SelectPayload(o.bytes);
    const size_t base_fds=Fds(),base_maps=Maps();
    if(o.role=="server") {
        uint64_t replies=0,opened=0,closed=0,send_failures=0;int active=0;
        {
            shm::EventLoop loop;Server server(&loop,o.path,"acceptance");
            server.SetCodecFactory([]{return std::make_shared<shm::codec::SimplePodCodec>();});
            server.SetConnectionCallback([&](const comm::TcpConnPtr&){++opened;++active;});
            server.SetCloseCallback([&](const comm::TcpConnPtr&){++closed;--active;REQUIRE(active>=0);std::printf("{\"role\":\"server_close\",\"time_ns\":%llu}\n",(unsigned long long)Now());std::fflush(stdout);});
            server.SetMessageCallback([&](const comm::TcpConnPtr& c,const std::any& p){if(!c->SendCmd(p)){++send_failures;c->Close();return;}++replies;});
            REQUIRE(server.Start());int timer=-1;
            REQUIRE(loop.AddTimer(1000,[&](int fd,short){shm::EventLoop::DrainTimerfd(fd);
                std::printf("{\"role\":\"server_tick\",\"active\":%d,\"opened\":%llu,\"closed\":%llu,\"replies\":%llu,\"send_failures\":%llu,\"fds\":%zu,\"ring_maps\":%zu}\n",active,(unsigned long long)opened,(unsigned long long)closed,(unsigned long long)replies,(unsigned long long)send_failures,Fds(),Maps());std::fflush(stdout);
                if(stopping)loop.Stop();},&timer)==shm::ErrorCode::kOk);
            std::puts("{\"role\":\"ready\"}");std::fflush(stdout);
            REQUIRE(loop.Run(100)==shm::ErrorCode::kOk);server.Stop();
        }
        REQUIRE(active==0&&Fds()==base_fds&&Maps()==base_maps);
        std::printf("{\"role\":\"server_final\",\"opened\":%llu,\"closed\":%llu,\"base_fds\":%zu,\"final_fds\":%zu,\"ring_maps\":%zu}\n",(unsigned long long)opened,(unsigned long long)closed,base_fds,Fds(),Maps());return 0;
    }
    REQUIRE(o.role=="client");
    bool completed=false,failed=false,closing=false,measuring=false,need_reconnect=false;
    int session=0,close_events=0;uint64_t sent=0,received=0,measured=0,start=0,end=0,phase_end=0,last_tick=0,full_checks=0;
    Histogram hist;std::vector<uint64_t> stamps(o.batch);
    std::any request=payload.make();
    {
        shm::EventLoop loop;Client client(&loop);
        client.SetCodecFactory([]{return std::make_shared<shm::codec::SimplePodCodec>();});
        auto batch=[&](const comm::TcpConnPtr& c) {
            uint64_t amount=o.batch;if(o.messages)amount=std::min<uint64_t>(amount,o.messages-sent);
            for(uint64_t i=0;i<amount;++i) {
                payload.set_seq(request,sent);
                if(measuring&&Sample(sent,o.stride))stamps[sent%o.batch]=Now();
                REQUIRE(c->SendCmd(request));++sent;
            }
        };
        client.SetConnectionCallback([&](const comm::TcpConnPtr& c){++session;need_reconnect=false;sent=received=measured=full_checks=0;hist=Histogram{};closing=false;measuring=o.warmup==0;start=Now();phase_end=start+uint64_t((measuring?o.seconds:o.warmup)*1e9);last_tick=start;std::printf("{\"role\":\"client_connected\",\"session\":%d,\"time_ns\":%llu}\n",session,(unsigned long long)Now());std::fflush(stdout);batch(c);});
        client.SetMessageCallback([&](const comm::TcpConnPtr& c,const std::any& p){
            uint64_t arrival=0;if(measuring&&Sample(received,o.stride))arrival=Now();
            bool full=Sample(received,64);
            uint64_t seq=payload.verify(p,full);
            REQUIRE(seq==received);if(full)++full_checks;
            if(measuring){++measured;if(arrival)hist.Add(arrival-stamps[received%o.batch]);}
            ++received;
            if(received!=sent)return;
            uint64_t now=Now();
            if(now-last_tick>=1000000000ULL) {std::printf("{\"role\":\"client_tick\",\"session\":%d,\"replies\":%llu}\n",session,(unsigned long long)received);std::fflush(stdout);last_tick=now;}
            if(o.messages?received>=o.messages:(measuring&&now>=phase_end)) {
                end=now;completed=true;closing=true;
                double seconds=double(end-start)/1e9;
                std::printf("{\"role\":\"result\",\"session\":%d,\"bytes\":%d,\"batch\":%d,\"gap_us\":%d,\"messages\":%llu,\"seconds\":%.9f,\"msg_s\":%.3f,\"samples\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"max_ns\":%llu,\"full_checks\":%llu,\"errors\":0}\n",session,o.bytes,o.batch,o.gap_us,(unsigned long long)measured,seconds,measured/seconds,(unsigned long long)hist.samples,(unsigned long long)hist.Percentile(.5),(unsigned long long)hist.Percentile(.99),(unsigned long long)hist.Percentile(.999),(unsigned long long)hist.max,(unsigned long long)full_checks);std::fflush(stdout);
                client.Close();return;
            }
            if(!measuring&&now>=phase_end){measuring=true;start=now;phase_end=now+uint64_t(o.seconds*1e9);}
            if(o.gap_us)std::this_thread::sleep_for(std::chrono::microseconds(o.gap_us));
            batch(c);
        });
        client.SetCloseCallback([&](const comm::TcpConnPtr&){++close_events;std::printf("{\"role\":\"client_close\",\"unexpected\":%s,\"time_ns\":%llu}\n",closing?"false":"true",(unsigned long long)Now());std::fflush(stdout);if(!closing){if(o.reconnect){need_reconnect=true;}else{failed=true;loop.Stop();}}
            else if(session<o.sessions&&!failed&&!stopping){completed=false;client.Connect(o.path);}else loop.Stop();});
        client.SetConnectErrorCallback([&](bool,const std::string&){if(o.reconnect)need_reconnect=true;else{failed=true;loop.Stop();}});
        int timer=-1;uint64_t watchdog=Now();
        REQUIRE(loop.AddTimer(1000,[&](int fd,short){shm::EventLoop::DrainTimerfd(fd);if(need_reconnect&&!stopping)client.Connect(o.path);if(stopping||Now()-watchdog>uint64_t((o.warmup+o.seconds+15)*o.sessions*1e9)){failed=true;loop.Stop();}},&timer)==shm::ErrorCode::kOk);
        client.Connect(o.path);if(!failed)REQUIRE(loop.Run(100)==shm::ErrorCode::kOk);
        closing=true;client.Close();
    }
    REQUIRE(Fds()==base_fds&&Maps()==base_maps);
    std::printf("{\"role\":\"client_final\",\"completed\":%s,\"failed\":%s,\"sessions\":%d,\"close_events\":%d,\"received\":%llu,\"base_fds\":%zu,\"final_fds\":%zu,\"ring_maps\":%zu}\n",completed?"true":"false",failed?"true":"false",session,close_events,(unsigned long long)received,base_fds,Fds(),Maps());
    return completed&&!failed?0:2;
}

int main(int argc,char** argv) {
    ::signal(SIGPIPE,SIG_IGN);::signal(SIGTERM,Signal);::signal(SIGINT,Signal);
    Options o;
    REQUIRE(argc>=3);o.role=argv[1];o.path=argv[2];
    for(int i=3;i<argc;++i) { REQUIRE(i+1<argc);std::string k=argv[i];const char* v=argv[++i];
        if(k=="--transport")o.transport=v;else if(k=="--socket-buffer")socket_bench::buffer_bytes=std::atoi(v);else if(k=="--socket-coalesce")socket_bench::coalesce=std::atoi(v)!=0;else if(k=="--single-type")o.single_type=std::atoi(v)!=0;else if(k=="--bytes")o.bytes=std::atoi(v);else if(k=="--batch")o.batch=std::atoi(v);
        else if(k=="--seconds")o.seconds=std::atof(v);else if(k=="--warmup")o.warmup=std::atof(v);
        else if(k=="--gap-us")o.gap_us=std::atoi(v);else if(k=="--sessions")o.sessions=std::atoi(v);
        else if(k=="--messages")o.messages=std::strtoull(v,nullptr,10);else if(k=="--stride")o.stride=std::atoi(v);else if(k=="--reconnect")o.reconnect=std::atoi(v);else REQUIRE(false);
    }
    REQUIRE(o.batch>0&&o.batch<=256&&o.sessions>0&&o.stride>0&&o.gap_us>=0&&o.seconds>0&&o.warmup>=0);
    REQUIRE(o.bytes!=1048576||o.batch<=4);
    const PayloadOps payload=SelectPayload(o.bytes);
    REQUIRE(uint64_t(o.bytes+4)*uint64_t(o.batch)<=5*1024*1024);
    if(o.single_type)payload.register_type();
    else {
        // Preserve legacy three-type scenarios and older supervisor commands.
        shm::codec::SimplePodCodec::RegisterPod<Small>();shm::codec::SimplePodCodec::RegisterPod<Big>();shm::codec::SimplePodCodec::RegisterPod<Huge>();
        if(o.bytes!=16&&o.bytes!=4096&&o.bytes!=1048576)payload.register_type();
    }
    REQUIRE(o.transport=="shm"||o.transport=="socket");
    REQUIRE(socket_bench::buffer_bytes>0);
    if(o.transport=="socket")return Run<socket_bench::Server,socket_bench::Client>(o);
    return Run<shm::ShmServer,shm::ShmAsyncClient>(o);
}
