// Production BatchWriter commits two separate stores with one publication.
#include <shm_ipc/ringbuf.hpp>
#include <pthread.h>
#include <cassert>
extern "C" void __VERIFIER_assume(int);
using Ring=shm::RingBuf<1024>;
alignas(64) static unsigned char region[Ring::kShmSize];
static void* writer(void*) {
    unsigned char first=17,second=29;
    Ring::BatchWriter batch(region);
    assert(batch.TryWrite(&first,1)==0);
    assert(batch.TryWrite(&second,1)==0);
    assert(batch.Flush()==2);
    return nullptr;
}
static void* reader(void*) {
    // Byte-wise reads avoid GenMC 0.10 mixed-size access limitations.
    unsigned char value=0;
    int rc=Ring::ReadExact(region,&value,1);
    __VERIFIER_assume(rc==0);
    assert(value==17);
    assert(Ring::ReadExact(region,&value,1)==0);
    assert(value==29);
    return nullptr;
}
int main() {
    auto* h=new(region) shm::RingHeader;
    h->write_pos.store(0,std::memory_order_relaxed);
    h->read_pos.store(0,std::memory_order_relaxed);
    h->reader_wait.store(shm::kReaderNotifyAlways,std::memory_order_relaxed);
    pthread_t r,w;
    pthread_create(&r,nullptr,reader,nullptr); pthread_create(&w,nullptr,writer,nullptr);
    pthread_join(r,nullptr); pthread_join(w,nullptr);
    assert(Ring::Available(region)==0);
}
