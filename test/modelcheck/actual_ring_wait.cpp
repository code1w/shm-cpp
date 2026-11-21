// Calls the production wait/notify methods. Initialized header is a precondition.
#include <shm_ipc/ringbuf.hpp>
#include <pthread.h>
#include <cassert>
using Ring=shm::RingBuf<1024>;
alignas(64) static unsigned char region[Ring::kShmSize];
static bool may_sleep,notify;
static void* reader(void*) {
    may_sleep=Ring::ArmReaderWait(region,0);
    return nullptr;
}
static void* writer(void*) {
    unsigned char value=17;
    assert(Ring::TryWrite(region,&value,1)==0);
    notify=Ring::ShouldNotify(region);
    return nullptr;
}
int main() {
    auto* h=new(region) shm::RingHeader;
    h->write_pos.store(0,std::memory_order_relaxed);
    h->read_pos.store(0,std::memory_order_relaxed);
    h->reader_wait.store(shm::kReaderAwake,std::memory_order_relaxed);
    pthread_t r,w;
    pthread_create(&r,nullptr,reader,nullptr); pthread_create(&w,nullptr,writer,nullptr);
    pthread_join(r,nullptr); pthread_join(w,nullptr);
    assert(!may_sleep || notify);
}
