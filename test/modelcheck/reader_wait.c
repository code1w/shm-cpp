/* Operation-for-operation C11 translation of ArmReaderWait/ShouldNotify.
 * One publication racing with one transition from empty to waiting.
 * Main observes results only after joins; it does not order worker operations.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef INITIAL_MODE
#define INITIAL_MODE 0
#endif
#ifndef READER_FENCE
#define READER_FENCE 1
#endif
#ifndef WRITER_FENCE
#define WRITER_FENCE 1
#endif
#ifndef FENCE_ORDER
#define FENCE_ORDER memory_order_seq_cst
#endif
static _Atomic uint64_t write_pos;
static _Atomic uint32_t reader_wait = INITIAL_MODE;
static int may_sleep, notify;
static void *reader(void *unused) {
    (void)unused;
    /* Empty read baseline is 0. */
    atomic_store_explicit(&reader_wait, 1, memory_order_relaxed);
#if READER_FENCE
    atomic_thread_fence(FENCE_ORDER);
#endif
    if (atomic_load_explicit(&write_pos, memory_order_acquire) != 0) {
        atomic_store_explicit(&reader_wait, 0, memory_order_relaxed);
        may_sleep = 0;
    } else may_sleep = 1;
    return 0;
}
static void *writer(void *unused) {
    (void)unused;
    atomic_store_explicit(&write_pos, 1, memory_order_release);
#if WRITER_FENCE
    atomic_thread_fence(FENCE_ORDER);
#endif
    uint32_t v = atomic_load_explicit(&reader_wait, memory_order_relaxed);
    if (v == 0) notify = 0;
    else if (v == 2) notify = 1;
    else notify = atomic_exchange_explicit(&reader_wait, 0, memory_order_acq_rel) != 0;
    return 0;
}
int main(void) {
    pthread_t r,w;
    pthread_create(&r,0,reader,0); pthread_create(&w,0,writer,0);
    pthread_join(r,0); pthread_join(w,0);
    assert(!may_sleep || notify); /* Pending publication cannot be left asleep. */
}
