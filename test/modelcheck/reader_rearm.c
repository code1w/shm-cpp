/* Two writes and two event-loop drain/arm cycles. Eventfd is abstracted as
 * an atomic notification counter (no data happens-before added by it).
 * Blocking wait is represented by assuming a counter exchange succeeds.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
void __VERIFIER_assume(int);
#ifndef READER_FENCE
#define READER_FENCE 1
#endif
#ifndef WRITER_FENCE
#define WRITER_FENCE 1
#endif
static _Atomic uint64_t write_pos,read_pos;
static _Atomic uint32_t reader_wait;
static _Atomic unsigned events;
static int final_sleep;
static int arm(uint64_t seen) {
    atomic_store_explicit(&reader_wait,1,memory_order_relaxed);
#if READER_FENCE
    atomic_thread_fence(memory_order_seq_cst);
#endif
    if(atomic_load_explicit(&write_pos,memory_order_acquire)!=seen) {
        atomic_store_explicit(&reader_wait,0,memory_order_relaxed);
        return 0;
    }
    return 1;
}
static int should_notify(void) {
#if WRITER_FENCE
    atomic_thread_fence(memory_order_seq_cst);
#endif
    uint32_t v=atomic_load_explicit(&reader_wait,memory_order_relaxed);
    if(v==0) return 0;
    if(v==2) return 1;
    return atomic_exchange_explicit(&reader_wait,0,memory_order_acq_rel)!=0;
}
static void *writer(void *unused) {
    (void)unused;
    for(uint64_t i=1;i<=2;++i) {
        atomic_store_explicit(&write_pos,i,memory_order_release);
        if(should_notify()) atomic_fetch_add_explicit(&events,1,memory_order_relaxed);
    }
    return 0;
}
static void *reader(void *unused) {
    (void)unused;
    atomic_exchange_explicit(&events,0,memory_order_relaxed); /* DrainNotify */
    uint64_t w=atomic_load_explicit(&write_pos,memory_order_acquire);
    atomic_store_explicit(&read_pos,w,memory_order_release);
    int sleep=arm(w);
    if(sleep) {
        unsigned n=atomic_exchange_explicit(&events,0,memory_order_relaxed);
        __VERIFIER_assume(n>0); /* successful poll, not a synchronizing gate */
    } else atomic_exchange_explicit(&events,0,memory_order_relaxed);
    w=atomic_load_explicit(&write_pos,memory_order_acquire);
    atomic_store_explicit(&read_pos,w,memory_order_release);
    final_sleep=arm(w);
    return 0;
}
int main(void) {
    pthread_t r,w;
    pthread_create(&r,0,reader,0); pthread_create(&w,0,writer,0);
    pthread_join(r,0); pthread_join(w,0);
    uint64_t pending=atomic_load_explicit(&write_pos,memory_order_relaxed)-atomic_load_explicit(&read_pos,memory_order_relaxed);
    assert(!final_sleep || !pending || atomic_load_explicit(&events,memory_order_relaxed)>0);
}
