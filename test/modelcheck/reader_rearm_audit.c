/* Audit model: capture baseline BEFORE draining, preserve incomplete frames,
 * and retain the still-blocked first-wait outcome (no assume).
 * A zero counter snapshot means return to the controller still sleeping;
 * the writer can complete later before the final property is inspected.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef FRAME_BYTES
#define FRAME_BYTES 2
#endif
#ifndef PUBLICATIONS
#define PUBLICATIONS 2
#endif
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
    for(uint64_t i=1;i<=PUBLICATIONS;++i) {
        atomic_store_explicit(&write_pos,i,memory_order_release);
        if(should_notify()) atomic_fetch_add_explicit(&events,1,memory_order_relaxed);
    }
    return 0;
}
static void *reader(void *unused) {
    (void)unused;
    for(int cycle=0;cycle<2;++cycle) {
        atomic_exchange_explicit(&events,0,memory_order_relaxed);
        uint64_t seen=atomic_load_explicit(&write_pos,memory_order_acquire);
        uint64_t observed=atomic_load_explicit(&write_pos,memory_order_acquire);
        // Abstract codec: only complete frames may advance read_pos.
        uint64_t consumed=observed-(observed%FRAME_BYTES);
        atomic_store_explicit(&read_pos,consumed,memory_order_release);
        final_sleep=arm(seen);
        if(final_sleep) {
            if(cycle==1) return 0;
            unsigned n=atomic_exchange_explicit(&events,0,memory_order_relaxed);
            if(n==0) return 0; // retain the pending/blocking wait execution
        }
    }
    return 0;
}
int main(void) {
    pthread_t r,w;
    pthread_create(&r,0,reader,0); pthread_create(&w,0,writer,0);
    pthread_join(r,0); pthread_join(w,0);
    uint64_t pending=atomic_load_explicit(&write_pos,memory_order_relaxed)-atomic_load_explicit(&read_pos,memory_order_relaxed);
    assert(!final_sleep || pending<FRAME_BYTES || atomic_load_explicit(&events,memory_order_relaxed)>0);
}
