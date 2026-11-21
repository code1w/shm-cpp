#include "poll_probe.hpp"
#include <atomic>
#include <cstdio>
#include <poll.h>
#include <cstdint>
namespace poll_probe {
struct alignas(64) Phase { std::atomic<bool> measured{false}; };
static Phase phase;
struct Counter {
 const char* role="other";
 uint64_t polls=0;
 ~Counter() { if (polls || role[0]!='o') std::fprintf(stderr,"probe role=%s poll_calls=%llu\n",role,(unsigned long long)polls); }
};
thread_local Counter count;
void SetRole(const char* role) { count.role=role; }
void StartMeasure() { phase.measured.store(true,std::memory_order_relaxed); }
}
extern "C" int __real_poll(pollfd*,nfds_t,int);
extern "C" int __wrap_poll(pollfd* fds,nfds_t count,int timeout) {
 if (poll_probe::phase.measured.load(std::memory_order_relaxed)) ++poll_probe::count.polls;
 return __real_poll(fds,count,timeout);
}
