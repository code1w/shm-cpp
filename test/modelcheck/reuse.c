/* Two-slot SPSC: fill, consume slot 0, reuse it while consuming slot 1.
 * Counter loads/stores retain production orders. Assume removes retry paths,
 * without creating happens-before edges or ordering data accesses.
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
void __VERIFIER_assume(int);
#ifndef RECLAIM_ORDER
#define RECLAIM_ORDER memory_order_release
#endif
#ifndef SPACE_ORDER
#define SPACE_ORDER memory_order_acquire
#endif
static _Atomic uint64_t write_pos,read_pos;
static int data[2];
static void *writer(void *unused) {
    (void)unused;
    for(uint64_t i=0;i<3;++i) {
        uint64_t w=atomic_load_explicit(&write_pos,memory_order_relaxed);
        uint64_t r=atomic_load_explicit(&read_pos,SPACE_ORDER);
        __VERIFIER_assume(w-r<2);
        data[w%2]=(int)i+1;
        atomic_store_explicit(&write_pos,w+1,memory_order_release);
    }
    return 0;
}
static void *reader(void *unused) {
    (void)unused;
    for(uint64_t i=0;i<3;++i) {
        uint64_t r=atomic_load_explicit(&read_pos,memory_order_relaxed);
        uint64_t w=atomic_load_explicit(&write_pos,memory_order_acquire);
        __VERIFIER_assume(w-r>0);
        assert(data[r%2]==(int)i+1);
        atomic_store_explicit(&read_pos,r+1,RECLAIM_ORDER);
    }
    return 0;
}
int main(void) {
    pthread_t r,w;
    pthread_create(&r,0,reader,0); pthread_create(&w,0,writer,0);
    pthread_join(r,0); pthread_join(w,0);
}
