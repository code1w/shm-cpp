#include "poll_probe.hpp"
#include <atomic>
#include <cstdio>
#include <poll.h>
#include <cstdint>
#include <unistd.h>
namespace poll_probe {
struct alignas(64) Phase { std::atomic<bool> measured{false}; };
static Phase phase;
struct Counter {
 const char* role="other";
 uint64_t polls=0,writes=0;
 ~Counter() { if (polls || writes || role[0]!='o') std::fprintf(stderr,"probe role=%s poll_calls=%llu notify_writes=%llu\n",role,(unsigned long long)polls,(unsigned long long)writes); }
};
thread_local Counter count;
void SetRole(const char* role) { count.role=role; }
void StartMeasure() { phase.measured.store(true,std::memory_order_relaxed); }
void EndMeasure() { phase.measured.store(false,std::memory_order_relaxed); }
}
extern "C" int __real_poll(pollfd*,nfds_t,int);
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __wrap_poll(pollfd* fds,nfds_t count,int timeout) {
 if (poll_probe::phase.measured.load(std::memory_order_relaxed)) ++poll_probe::count.polls;
 return __real_poll(fds,count,timeout);
}
extern "C" ssize_t __wrap_write(int fd,const void* buf,size_t size) {
 if(size==8 && poll_probe::phase.measured.load(std::memory_order_relaxed)) ++poll_probe::count.writes;
 return __real_write(fd,buf,size);
}
