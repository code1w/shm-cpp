from pathlib import Path
import shutil,subprocess,concurrent.futures,difflib
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_notify_startup';out.mkdir(exist_ok=True)
root=Path('/tmp/shm-notify-startup');root.mkdir(exist_ok=True)
variants=['baseline','current','arm_early','force_client_always']
original=Path('/tmp/shm-ca766b1-vs-current/after/bench/bench_app.cpp').read_text()
bench=original.replace('#include <unistd.h>','#include <unistd.h>\n#include "poll_probe.hpp"')
bench=bench.replace('g_t0            = NowNs();','g_t0            = NowNs();\n        poll_probe::StartMeasure();')
bench=bench.replace('const uint64_t elapsed = NowNs() - g_t0;','const uint64_t elapsed = NowNs() - g_t0;\n            poll_probe::EndMeasure();\n            fprintf(stderr, "probe reader_mode=%u\\n", static_cast<shm::ShmConnection*>(conn.get())->Channel().ProbeReaderMode());')
bench=bench.replace('std::thread server_thread([&] {','std::thread server_thread([&] {\n        poll_probe::SetRole("server");').replace('std::thread client_thread([&] {','std::thread client_thread([&] {\n        poll_probe::SetRole("client");')
probe_h='''#pragma once
namespace poll_probe { void StartMeasure(); void EndMeasure(); void SetRole(const char*); }
'''
probe_cpp='''#include "poll_probe.hpp"
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
 ~Counter() { if (polls || writes || role[0]!='o') std::fprintf(stderr,"probe role=%s poll_calls=%llu notify_writes=%llu\\n",role,(unsigned long long)polls,(unsigned long long)writes); }
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
'''
oldroot=Path('/tmp/shm-ca766b1-vs-current/before/include')
for v in variants:
 d=root/v;d.mkdir(exist_ok=True)
 shutil.copytree(oldroot if v=='baseline' else repo/'include',d/'include',dirs_exist_ok=True)
 (d/'bench_app.cpp').write_text(bench);(d/'poll_probe.hpp').write_text(probe_h);(d/'poll_probe.cpp').write_text(probe_cpp)
 cmake=Path('/tmp/shm-contract-perf/CMakeLists.txt').read_text().replace('add_executable(bench_app bench_app.cpp)','add_executable(bench_app bench_app.cpp poll_probe.cpp)\ntarget_link_options(bench_app PRIVATE -Wl,--wrap=poll -Wl,--wrap=write)')
 (d/'CMakeLists.txt').write_text(cmake)
 p=d/'include/shm_ipc/ring_channel.hpp';s=p.read_text();i=s.index('    int NotifyReadFd()');s=s[:i]+'''    uint32_t ProbeReaderMode() const
    {
        return read_region_ ? detail::RingHdr(read_region_.Get())->reader_wait.load(std::memory_order_relaxed) : UINT32_MAX;
    }

'''+s[i:];p.write_text(s)
 if v in ['arm_early','force_client_always']:
  p=d/'include/shm_ipc/shm_connection.hpp';s=p.read_text();orig=s
  if v=='arm_early':
   s=s.replace('''    void OnConnection()
    {
        AssertCodecIdle();''','''    void OnConnection()
    {
        AssertCodecIdle();
        if (!channel_.PrepareWait(channel_.ReadPos())) channel_.WakeReader();''')
  else:
   s=s.replace('if (channel_.PrepareWait(seen))','if (conntype_ == comm::TcpConnType::TcpConn_Client ? channel_.WritePos() == seen : channel_.PrepareWait(seen))')
  assert s!=orig,v;p.write_text(s)
  (out/(v+'.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/shm_connection.hpp',tofile=v+'/shm_connection.hpp')))
(out/'prepare.py').write_text(Path('/tmp/prepare-shm-notify-startup.py').read_text());(out/'poll_probe.hpp').write_text(probe_h);(out/'poll_probe.cpp').write_text(probe_cpp)
(out/'env.txt').write_text(subprocess.check_output(['uname','-a'],text=True)+subprocess.check_output(['c++','--version'],text=True)+subprocess.check_output(['lscpu'],text=True)+'\nRelease -O2 -DNDEBUG; fixed server CPU2/client CPU4; original 1,000,000 reply benchmark; eventfd writes counted in user code; no warmup.\n')
def build(v):
 d=str(root/v)
 with (out/(v+'_build.log')).open('w') as f:
  subprocess.run(['cmake','-S',d,'-B',d+'/build','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG'],stdout=f,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['cmake','--build',d+'/build','-j2'],stdout=f,stderr=subprocess.STDOUT,check=True)
 return v
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 for v in ex.map(build,variants):print('built',v,flush=True)
