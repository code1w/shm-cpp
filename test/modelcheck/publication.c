/* Abstract two-slot SPSC batch. Data stores are intentionally non-atomic.
 * Geometry/copying is checked separately by ringbuf_model; this model checks
 * the release/acquire edge that publishes the whole batch.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
void __VERIFIER_assume(int);
#ifndef PUBLISH_ORDER
#define PUBLISH_ORDER memory_order_release
#endif
#ifndef OBSERVE_ORDER
#define OBSERVE_ORDER memory_order_acquire
#endif
static _Atomic uint64_t write_pos;
static int data[2];
static void *writer(void *unused) {
    (void)unused;
    uint64_t w=atomic_load_explicit(&write_pos,memory_order_relaxed);
    data[0]=17; data[1]=29;
    atomic_store_explicit(&write_pos,w+2,PUBLISH_ORDER); /* BatchWriter::Flush */
    return 0;
}
static void *reader(void *unused) {
    (void)unused;
    uint64_t w=atomic_load_explicit(&write_pos,OBSERVE_ORDER);
    __VERIFIER_assume(w==2); /* Analyze successful read path, not a retry loop. */
    assert(data[0]==17 && data[1]==29);
    return 0;
}
int main(void) {
    pthread_t r,w;
    pthread_create(&r,0,reader,0); pthread_create(&w,0,writer,0);
    pthread_join(r,0); pthread_join(w,0);
}
