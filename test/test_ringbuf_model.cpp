// Sequential refinement tests. The oracle holds logical byte sequences and
// non-wrapping operation totals; it does not implement ring indexing.
#include "ringbuf_test_fixture.hpp"
#include <algorithm>
#include <deque>
#include <memory>
#include <random>
#include <vector>

static uint64_t active_seed;
static unsigned active_step;
static size_t active_capacity;
static bool active_mirror;
static uint64_t active_origin;
#define MODEL_CHECK(x) do { if (!(x)) { std::fprintf(stderr,"seed=%llu step=%u capacity=%zu mirror=%d origin=%llu\n", (unsigned long long)active_seed, active_step, active_capacity, active_mirror, (unsigned long long)active_origin); std::fprintf(stderr,"FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

template<size_t C>
void Run(bool mirror, uint64_t seed, uint64_t origin) {
    using R = shm::RingBuf<C>;
    RingFixture<C> f(mirror, origin);
    std::deque<unsigned char> visible;
    std::vector<unsigned char> pending;
    std::unique_ptr<typename R::BatchWriter> batch;
    uint64_t written = 0, consumed = 0, snapshot_read = 0;
    int count = 0;
    std::mt19937_64 rng(seed);
    active_seed = seed; active_capacity=C; active_mirror=mirror; active_origin=origin;
    auto verify = [&] {
        MODEL_CHECK(R::Available(f.Get()) == visible.size());
        MODEL_CHECK(R::FreeSpace(f.Get()) == C - visible.size());
        MODEL_CHECK(R::WritePos(f.Get()) == origin + written);
        MODEL_CHECK(R::ReadPos(f.Get()) == origin + consumed);
        const void *a=nullptr,*b=nullptr; uint32_t na=0,nb=0;
        int rc=R::Peek(f.Get(),&a,&na,&b,&nb);
        MODEL_CHECK(rc == (visible.empty() ? -1 : 0));
        if (!visible.empty()) {
            MODEL_CHECK(na + nb == visible.size());
            for(size_t i=0;i<visible.size();++i)
                MODEL_CHECK(visible[i] == (i<na ? static_cast<const unsigned char*>(a)[i] : static_cast<const unsigned char*>(b)[i-na]));
            if(mirror) {
                const void* p=nullptr; uint32_t n=0;
                MODEL_CHECK(R::PeekContig(f.Get(),&p,&n)==0 && n==visible.size());
                MODEL_CHECK(std::equal(visible.begin(),visible.end(),static_cast<const unsigned char*>(p)));
            }
        }
        if(batch) {
            MODEL_CHECK(batch->PendingBytes()==pending.size());
            MODEL_CHECK(batch->Count()==count);
            MODEL_CHECK(batch->FreeBytes()==C-(written+pending.size()-snapshot_read));
        }
    };
    auto publish = [&] {
        visible.insert(visible.end(),pending.begin(),pending.end());
        written += pending.size(); pending.clear(); count=0;
    };
    for(active_step=0;active_step<2000;++active_step) {
        const unsigned op=rng()%12;
        // Bias lengths toward discontinuities, including empty/invalid requests.
        const uint32_t edges[]={0,1,7,8,uint32_t(C/2),uint32_t(C/2+1),uint32_t(C),uint32_t(C+1)};
        uint32_t len=(rng()%2) ? edges[rng()%8] : rng()%(C+2);
        std::vector<unsigned char> data(len);
        for(auto& v:data) v=static_cast<unsigned char>(rng());
        if(op==0 && !batch) {
            batch=std::make_unique<typename R::BatchWriter>(f.Get(),mirror);
            snapshot_read=consumed;
        } else if(op<=2) {
            size_t used=batch ? written+pending.size()-snapshot_read : visible.size();
            bool ok=len>0 && len<=C/2 && len<=C-used;
            int rc=batch ? batch->TryWrite(data.data(),len) : R::TryWrite(f.Get(),data.data(),len);
            MODEL_CHECK(rc==(ok?0:-1));
            if(ok) {
                if(batch) { pending.insert(pending.end(),data.begin(),data.end()); ++count; }
                else { visible.insert(visible.end(),data.begin(),data.end()); written+=len; }
            }
        } else if(op==3 && batch) {
            size_t used=written+pending.size()-snapshot_read;
            // Physical-tail calculation is only used for the API's contiguous
            // reservation precondition, never for oracle byte placement.
            size_t tail=C-((origin+written+pending.size())%C);
            bool ok=len>0 && len<=C/2 && len<=C-used && (mirror || len<=tail);
            char* p=batch->Reserve(len);
            MODEL_CHECK((p!=nullptr)==ok);
            if(p) {
                std::copy(data.begin(),data.end(),p);
                if(rng()%3==0) { // rejected commit must preserve pending state
                    batch->CommitReserve(len+1);
                    verify();
                }
                batch->CommitReserve(len);
                pending.insert(pending.end(),data.begin(),data.end()); ++count;
            }
        } else if(op==4 && batch) {
            MODEL_CHECK(batch->Flush()==count); publish();
            MODEL_CHECK(batch->Flush()==0);
        } else if(op==5 && batch) {
            batch->Cancel(); pending.clear(); count=0;
        } else if(op==6 && batch) {
            batch.reset(); publish(); // destructor must publish exactly once
        } else if(op==10 && batch) {
            // Rewind restores a caller-supplied prior byte/count checkpoint.
            batch->RewindTo(0,0); pending.clear(); count=0;
            MODEL_CHECK(batch->PendingBytes()==0);
        } else if(op==11 && batch) {
            typename R::BatchWriter moved(std::move(*batch));
            MODEL_CHECK(batch->TryWrite(data.data(),len)==-1);
            MODEL_CHECK(batch->Flush()==0);
            *batch=std::move(moved);
        } else if(op>=7 && op<=9) {
            std::vector<unsigned char> out(len,0xD3);
            size_t n=std::min<size_t>(len,visible.size());
            if(op==7) {
                int rc=R::ReadExact(f.Get(),out.data(),len);
                bool ok=len<=visible.size();
                MODEL_CHECK(rc==(ok?0:-1)); n=ok?len:0;
                if(!ok) MODEL_CHECK(std::all_of(out.begin(),out.end(),[](unsigned char v){return v==0xD3;}));
            } else if(op==8) {
                MODEL_CHECK(R::TryRead(f.Get(),out.data(),len)==n);
            } else {
                std::copy_n(visible.begin(),n,out.begin());
                R::CommitRead(f.Get(),n);
            }
            for(size_t i=0;i<n;++i) { MODEL_CHECK(out[i]==visible.front()); visible.pop_front(); }
            consumed+=n;
        }
        verify();
    }
    if(batch) { batch.reset(); publish(); verify(); }
}

template<size_t C>
void OverflowCases(bool mirror) {
    using R=shm::RingBuf<C>;
    // Cross numerical and physical boundaries together, from both sides.
    for(uint64_t distance : {uint64_t(1),uint64_t(8),uint64_t(C/2),uint64_t(C),uint64_t(C+1)}) {
        RingFixture<C> f(mirror,UINT64_MAX-distance+1);
        uint64_t origin=R::WritePos(f.Get());
        MODEL_CHECK(R::ArmReaderWait(f.Get(),origin));
        std::vector<unsigned char> data(C/2,0xA7), out(C);
        typename R::BatchWriter b(f.Get(),mirror);
        MODEL_CHECK(b.TryWrite(data.data(),data.size())==0);
        MODEL_CHECK(R::Available(f.Get())==0); // wrap cannot publish early
        MODEL_CHECK(b.PendingBytes()==data.size());
        b.Cancel(); MODEL_CHECK(b.Flush()==0 && R::WritePos(f.Get())==origin);
        MODEL_CHECK(b.TryWrite(data.data(),data.size())==0);
        MODEL_CHECK(b.TryWrite(data.data(),data.size())==0);
        MODEL_CHECK(b.Flush()==2 && R::Available(f.Get())==C);
        MODEL_CHECK(R::ShouldNotify(f.Get()));
        MODEL_CHECK(!R::ShouldNotify(f.Get()));
        MODEL_CHECK(!R::ArmReaderWait(f.Get(),origin));
        MODEL_CHECK(R::TryWrite(f.Get(),data.data(),1)==-1);
        MODEL_CHECK(R::ReadExact(f.Get(),out.data(),C)==0);
        MODEL_CHECK(std::all_of(out.begin(),out.end(),[](unsigned char v){return v==0xA7;}));
        MODEL_CHECK(R::ReadPos(f.Get())==origin+C && R::FreeSpace(f.Get())==C);
        MODEL_CHECK(R::ArmReaderWait(f.Get(),R::ReadPos(f.Get())));
    }
}

template<size_t C> void SnapshotAndReserve(bool mirror) {
    using R=shm::RingBuf<C>;
    // Exactly 64 bytes remain before both uint64 and physical wrap.
    RingFixture<C> f(mirror,UINT64_MAX-63);
    {
        typename R::BatchWriter b(f.Get(),mirror);
        char* p=b.Reserve(64); MODEL_CHECK(p!=nullptr);
        std::fill_n(p,64,0x36);
        b.CommitReserve(63); MODEL_CHECK(b.PendingBytes()==0);
        b.CommitReserve(64); MODEL_CHECK(b.PendingBytes()==64);
        MODEL_CHECK(R::WritePos(f.Get())==UINT64_MAX-63);
        MODEL_CHECK(b.Flush()==1 && R::WritePos(f.Get())==0);
        std::vector<unsigned char> got(64);
        int rc=mirror ? R::ReadExactContig(f.Get(),got.data(),64) : R::ReadExact(f.Get(),got.data(),64);
        MODEL_CHECK(rc==0 && std::all_of(got.begin(),got.end(),[](unsigned char v){return v==0x36;}));
    }
    {
        typename R::BatchWriter checkpoint(f.Get(),mirror);
        std::vector<unsigned char> first(64,0x21), second(64,0xE2);
        MODEL_CHECK(checkpoint.TryWrite(first.data(),64)==0);
        const auto bytes=checkpoint.PendingBytes(); const int calls=checkpoint.Count();
        MODEL_CHECK(checkpoint.TryWrite(second.data(),64)==0);
        checkpoint.RewindTo(bytes,calls);
        MODEL_CHECK(checkpoint.PendingBytes()==64 && checkpoint.Count()==1);
        MODEL_CHECK(checkpoint.Flush()==1);
        std::vector<unsigned char> out(64);
        MODEL_CHECK(R::ReadExact(f.Get(),out.data(),64)==0 && out==first);
        MODEL_CHECK(R::Available(f.Get())==0);
    }
    // A live writer's read snapshot must remain conservative after reclamation.
    std::vector<unsigned char> data(C/2,0x71), all(C);
    // Reconstruct a fresh writer before filling the ring.
    {
        typename R::BatchWriter fill(f.Get(),mirror);
        MODEL_CHECK(fill.TryWrite(data.data(),data.size())==0);
        MODEL_CHECK(fill.TryWrite(data.data(),data.size())==0);
        MODEL_CHECK(fill.Flush()==2);
        MODEL_CHECK(R::ReadExact(f.Get(),all.data(),C)==0);
        MODEL_CHECK(fill.TryWrite(data.data(),1)==-1); // stale snapshot says full
        // End the writer's active scope before constructing a replacement writer.
        fill.Cancel();
    }
    typename R::BatchWriter fresh(f.Get(),mirror);
    MODEL_CHECK(fresh.TryWrite(data.data(),1)==0);
    MODEL_CHECK(fresh.Flush()==1);
    unsigned char value=0;
    MODEL_CHECK(R::ReadExact(f.Get(),&value,1)==0 && value==0x71);
}

template<size_t C> void Suite(uint64_t seed) {
    for(bool mirror : {false,true}) {
        // Linux mirrored mapping requires page-aligned capacities.
        if(mirror && C<4096) continue;
        OverflowCases<C>(mirror);
        SnapshotAndReserve<C>(mirror);
        for(unsigned i=0;i<12;++i) {
            Run<C>(mirror,seed+i,0);
            Run<C>(mirror,seed+i,UINT64_MAX-C/2);
        }
    }
}
int main(int argc,char** argv) {
    uint64_t seed=argc>1 ? std::strtoull(argv[1],nullptr,0) : 0x5A17;
    Suite<1024>(seed); Suite<4096>(seed); Suite<8192>(seed);
    std::puts("PASS: overflow and sequential byte-queue refinement (seed replay supported)");
}
