// Execute the production header, not its C11 translation. Byte size is kept
// small for event-graph tractability; full geometry has separate model tests.
#include <shm_ipc/ringbuf.hpp>
#include <pthread.h>
#include <cassert>
extern "C" void __VERIFIER_assume(int);
using Ring=shm::RingBuf<1024>;
alignas(64) static unsigned char region[Ring::kShmSize];
static void* writer(void*) {
    unsigned char value=0xA7;
    assert(Ring::TryWrite(region,&value,1)==0);
    return nullptr;
}
static void* reader(void*) {
    unsigned char value=0;
    int rc=Ring::ReadExact(region,&value,1);
    __VERIFIER_assume(rc==0);
    assert(value==0xA7);
    return nullptr;
}
int main() {
    // Start header lifetime and initialize atomics explicitly. GenMC 0.10
    // cannot interpret Init's page-sized memset with LLVM opaque pointers.
    // Initialization/mapping itself is covered by the process tests.
    auto* h=new(region) shm::RingHeader;
    h->write_pos.store(0,std::memory_order_relaxed);
    h->read_pos.store(0,std::memory_order_relaxed);
    h->reader_wait.store(shm::kReaderNotifyAlways,std::memory_order_relaxed);
    pthread_t r,w;
    pthread_create(&r,nullptr,reader,nullptr);
    pthread_create(&w,nullptr,writer,nullptr);
    pthread_join(r,nullptr); pthread_join(w,nullptr);
    assert(Ring::Available(region)==0);
}
