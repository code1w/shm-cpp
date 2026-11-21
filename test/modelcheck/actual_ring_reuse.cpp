// Initially full production ring. Reader consumes byte 0.
// Writer must acquire reclaimed space before reusing byte 0.
#include <shm_ipc/ringbuf.hpp>
#include <pthread.h>
#include <cassert>
extern "C" void __VERIFIER_assume(int);
using Ring=shm::RingBuf<1024>;
alignas(64) static unsigned char region[Ring::kShmSize];
static void* writer(void*) {
    unsigned char value=29;
    int rc=Ring::TryWrite(region,&value,1);
    __VERIFIER_assume(rc==0);
    return nullptr;
}
static void* reader(void*) {
    unsigned char value=0;
    assert(Ring::ReadExact(region,&value,1)==0);
    assert(value==17);
    return nullptr;
}
int main() {
    auto* h=new(region) shm::RingHeader;
    h->write_pos.store(1024,std::memory_order_relaxed);
    h->read_pos.store(0,std::memory_order_relaxed);
    h->reader_wait.store(shm::kReaderNotifyAlways,std::memory_order_relaxed);
    region[shm::kRingDataOffset]=17; // initial publication precedes thread creation
    pthread_t r,w;
    pthread_create(&r,nullptr,reader,nullptr); pthread_create(&w,nullptr,writer,nullptr);
    pthread_join(r,nullptr); pthread_join(w,nullptr);
    assert(Ring::Available(region)==1024);
    assert(region[shm::kRingDataOffset]==29);
}
