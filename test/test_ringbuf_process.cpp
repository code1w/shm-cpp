// Targeted Linux process tests, not a weak-memory proof. Control UDS schedules
// selected boundary orderings; race mode does not synchronize after publication.
#include "ringbuf_test_fixture.hpp"
#include <shm_ipc/ring_channel.hpp>
#include <dirent.h>
#include <fstream>
#include <string>
#include <array>
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

using R=shm::RingBuf<4096>;
using F=RingFixture<4096>;

void Send(int fd,char token) { RB_CHECK(::write(fd,&token,1)==1); }
char Receive(int fd) { char token=0; RB_CHECK(::read(fd,&token,1)==1); return token; }
void WaitChild(pid_t pid) {
    int status=0;
    RB_CHECK(::waitpid(pid,&status,0)==pid);
    RB_CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
std::array<uint64_t,16> Payload(uint64_t sequence) {
    std::array<uint64_t,16> p{};
    p[0]=sequence;
    for(size_t i=1;i<p.size();++i) p[i]=(sequence*0x9e3779b97f4a7c15ULL) ^ (i*0xd6e8feb86659fd93ULL);
    return p;
}
void Notify(F& f,int efd) {
    if(R::ShouldNotify(f.Get())) {
        uint64_t one=1; RB_CHECK(::write(efd,&one,8)==8);
    }
}
void Await(int efd) {
    pollfd p{efd,POLLIN,0};
    RB_CHECK(::poll(&p,1,2000)==1 && (p.revents&POLLIN));
    uint64_t value=0; RB_CHECK(::read(efd,&value,8)==8 && value>0);
}
void IndependentMapping(F& f) {
    // Keep the inherited VA occupied while making a second mapping, ensuring
    // the child actually accesses this memfd through a distinct address.
    shm::MmapRegion inherited=std::move(f.region);
    f.Map(); RB_CHECK(f.Get()!=inherited.Get());
}

void WakeSchedule(bool mirror,int order) {
    F f(mirror,UINT64_MAX-63);
    shm::UniqueFd event;
    RB_CHECK(shm::CreateEventfd(&event)==shm::ErrorCode::kOk);
    int uds[2]; RB_CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,uds)==0);
    shm::UniqueFd parent(uds[0]),child(uds[1]);
    pid_t pid=::fork(); RB_CHECK(pid>=0);
    if(pid==0) {
        ::alarm(15); parent.Reset(); IndependentMapping(f);
        for(uint64_t seq=0;seq<128;++seq) {
            RB_CHECK(Receive(child.Get())=='G');
            auto data=Payload(seq);
            typename R::BatchWriter b(f.Get(),mirror);
            // Deliberately split one record into two writes. Only Flush publishes.
            RB_CHECK(b.TryWrite(data.data(),56)==0);
            RB_CHECK(b.TryWrite(reinterpret_cast<char*>(data.data())+56,sizeof(data)-56)==0);
            RB_CHECK(b.Flush()==2);
            Notify(f,event.Get());
            if(order==0) Send(child.Get(),'P');
            RB_CHECK(Receive(child.Get())=='A');
        }
        ::_exit(0);
    }
    child.Reset();
    for(uint64_t seq=0;seq<128;++seq) {
        uint64_t seen=R::ReadPos(f.Get());
        RB_CHECK(R::Available(f.Get())==0);
        bool sleep=false;
        if(order==1) { // waiter armed before producer may publish
            RB_CHECK(R::ArmReaderWait(f.Get(),seen));
            sleep=true; Send(parent.Get(),'G');
        } else {
            Send(parent.Get(),'G');
            if(order==0) RB_CHECK(Receive(parent.Get())=='P');
            // order=0: publish-before-arm; order=2: race after start gate.
            sleep=R::ArmReaderWait(f.Get(),seen);
            if(order==0) RB_CHECK(!sleep);
        }
        if(sleep) Await(event.Get());
        else { // remove a permitted redundant wake from the preceding phase
            uint64_t value; ssize_t n=::read(event.Get(),&value,8);
            RB_CHECK(n==8 || (n==-1 && errno==EAGAIN));
        }
        std::array<uint64_t,16> got{};
        RB_CHECK(R::ReadExact(f.Get(),got.data(),sizeof(got))==0);
        RB_CHECK(got==Payload(seq));
        RB_CHECK(R::Available(f.Get())==0);
        Send(parent.Get(),'A');
    }
    char c; RB_CHECK(::read(parent.Get(),&c,1)==0); // process exit closes UDS
    WaitChild(pid);
}

void ExitAtCommit(bool mirror,bool published) {
    int old_fd=-1; void* old_address=nullptr;
    {
        F f(mirror,UINT64_MAX-63);
        old_fd=f.fd.Get(); old_address=f.Get();
        shm::UniqueFd event;
        RB_CHECK(shm::CreateEventfd(&event)==shm::ErrorCode::kOk);
        RB_CHECK(R::ArmReaderWait(f.Get(),R::ReadPos(f.Get())));
        int uds[2]; RB_CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,uds)==0);
        shm::UniqueFd parent(uds[0]),child(uds[1]);
        pid_t pid=::fork(); RB_CHECK(pid>=0);
        if(pid==0) {
            ::alarm(15); parent.Reset(); IndependentMapping(f);
            auto data=Payload(91);
            typename R::BatchWriter b(f.Get(),mirror);
            RB_CHECK(b.TryWrite(data.data(),sizeof(data))==0);
            if(published) RB_CHECK(b.Flush()==1);
            // _exit skips C++ destructors. No notification in either case.
            ::_exit(0);
        }
        child.Reset();
        char c; RB_CHECK(::read(parent.Get(),&c,1)==0);
        WaitChild(pid);
        uint64_t notification=0;
        RB_CHECK(::read(event.Get(),&notification,8)==-1 && errno==EAGAIN);
        std::array<uint64_t,16> got{};
        RB_CHECK(R::Available(f.Get())==(published?sizeof(got):0));
        RB_CHECK(R::ReadExact(f.Get(),got.data(),sizeof(got))==(published?0:-1));
        if(published) RB_CHECK(got==Payload(91));
        // Parent's reference preserves the mapping after producer termination.
        unsigned char residency=0;
        RB_CHECK(::mincore(f.Get(),4096,&residency)==0);
    }
    // No allocation or fd creation between destruction and these checks.
    RB_CHECK(::fcntl(old_fd,F_GETFD)==-1 && errno==EBADF);
    unsigned char residency=0;
    RB_CHECK(::mincore(old_address,4096,&residency)==-1 && errno==ENOMEM);
    // A new session must be empty, even after a preceding unpublished write.
    F replacement(mirror);
    RB_CHECK(R::WritePos(replacement.Get())==0 && R::Available(replacement.Get())==0);
}
size_t ChannelMappings() {
    std::ifstream maps("/proc/self/maps"); RB_CHECK(maps.good());
    std::string line; size_t count=0;
    while(std::getline(maps,line))
        if(line.find("memfd:client_to_server")!=std::string::npos ||
           line.find("memfd:server_to_client")!=std::string::npos) ++count;
    return count;
}
size_t OpenFds() {
    DIR* dir=::opendir("/proc/self/fd"); RB_CHECK(dir!=nullptr);
    size_t n=0;
    while(auto* entry=::readdir(dir)) if(entry->d_name[0]!='.') ++n;
    RB_CHECK(::closedir(dir)==0); return n;
}
void HandshakeExitAndNewSession() {
    using Channel=shm::RingChannel<4096>;
    const size_t initial_fds=OpenFds(),initial_maps=ChannelMappings();
    // Each iteration creates rings after fork: receiver obtains them solely by
    // production SCM_RIGHTS handshake, never through inheritance.
    for(unsigned generation=0;generation<3;++generation) {
        {
            int uds[2]; RB_CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,uds)==0);
            shm::UniqueFd parent(uds[0]),child(uds[1]);
            pid_t pid=::fork(); RB_CHECK(pid>=0);
            if(pid==0) {
                ::alarm(15); parent.Reset();
                Channel channel;
                RB_CHECK(Channel::CreateRingChannel(child.Get(),shm::ChannelRole::kServer,&channel)==shm::ErrorCode::kOk);
                RB_CHECK(channel.Readable()==0 && channel.WritableBytes()==4096);
                RB_CHECK(Receive(child.Get())=='G');
                auto data=Payload(generation);
                if(generation==1) {
                    Channel::ChannelBatchWriter batch;
                    RB_CHECK(channel.StartBatch(&batch)==shm::ErrorCode::kOk);
                    RB_CHECK(batch.TryWrite(data.data(),sizeof(data))==0);
                    ::_exit(0); // no Flush/destructor: unpublished record dies
                }
                RB_CHECK(channel.TryWrite(data.data(),sizeof(data))==0);
                if(generation==0) channel.NotifyPeer();
                // generation 2: published data, process dies before notifying.
                ::_exit(0);
            }
            child.Reset(); Channel channel;
            RB_CHECK(Channel::CreateRingChannel(parent.Get(),shm::ChannelRole::kClient,&channel)==shm::ErrorCode::kOk);
            RB_CHECK(channel.Readable()==0 && channel.WritableBytes()==4096);
            RB_CHECK(ChannelMappings()>initial_maps);
            RB_CHECK(channel.PrepareWait(channel.ReadPos()));
            Send(parent.Get(),'G');
            char token=0; RB_CHECK(::read(parent.Get(),&token,1)==0);
            WaitChild(pid);
            std::array<uint64_t,16> got{};
            RB_CHECK(channel.Readable()==(generation==1?0:sizeof(got)));
            RB_CHECK(channel.ReadExact(got.data(),sizeof(got))==(generation==1?-1:0));
            if(generation!=1) RB_CHECK(got==Payload(generation));
            RB_CHECK(channel.Readable()==0);
        }
        RB_CHECK(OpenFds()==initial_fds);
        RB_CHECK(ChannelMappings()==initial_maps);
    }
}

int main() {
    ::alarm(30); // bound deadlocks in both controller and worker
    for(bool mirror:{false,true}) {
        for(int order=0;order<3;++order) WakeSchedule(mirror,order);
        ExitAtCommit(mirror,false);
        ExitAtCommit(mirror,true);
    }
    HandshakeExitAndNewSession();
    std::puts("PASS: independent process mappings, wake boundaries, commit/exit and local resource release");
}
