from pathlib import Path
import shutil,subprocess,concurrent.futures,re,difflib
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_residual_attribution';out.mkdir(exist_ok=True)
root=Path('/tmp/shm-residual-attribution');root.mkdir(exist_ok=True)
variants=['baseline','current','old_loop','old_ring','old_read','direct_dispatch','hs_watch_off','old_loop_hs_off']
original=Path('/tmp/shm-ca766b1-vs-current/after/bench/bench_app.cpp').read_text()
bench=original.replace('int g_total      = 1000000;', 'constexpr int kWarmup = 1 << 20;\nconstexpr int kMeasured = 8 * kWarmup;\nint g_total = kWarmup + kMeasured;')
assert bench!=original
bench=bench.replace('#include <unistd.h>','#include <unistd.h>\n#include "poll_probe.hpp"')
bench=bench.replace('''        if (n >= g_total)
        {''','''        if (n == kWarmup)
        {
            g_t0 = NowNs();
            poll_probe::StartMeasure();
        }
        if (n >= g_total)
        {''')
bench=bench.replace('static_cast<double>(g_total) * KindPayloadSize()', 'static_cast<double>(kMeasured) * KindPayloadSize()').replace('KindName(), g_batch, g_total, secs, g_total / secs, mbps);','KindName(), g_batch, kMeasured, secs, kMeasured / secs, mbps);')
bench=bench.replace('std::thread server_thread([&] {','std::thread server_thread([&] {\n        poll_probe::SetRole("server");').replace('std::thread client_thread([&] {','std::thread client_thread([&] {\n        poll_probe::SetRole("client");')
(out/'warmup_and_probe.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),bench.splitlines(True),fromfile='pinned/bench_app.cpp',tofile='warm/bench_app.cpp')))
probe_h='''#pragma once
namespace poll_probe { void StartMeasure(); void SetRole(const char*); }
'''
probe_cpp='''#include "poll_probe.hpp"
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
 ~Counter() { if (polls || role[0]!='o') std::fprintf(stderr,"probe role=%s poll_calls=%llu\\n",role,(unsigned long long)polls); }
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
'''
oldroot=Path('/tmp/shm-ca766b1-vs-current/before/include')
oldconn=(oldroot/'shm_ipc/shm_connection.hpp').read_text()
for v in variants:
 d=root/v;d.mkdir(exist_ok=True)
 shutil.copytree(oldroot if v=='baseline' else repo/'include',d/'include',dirs_exist_ok=True)
 (d/'bench_app.cpp').write_text(bench);(d/'poll_probe.hpp').write_text(probe_h);(d/'poll_probe.cpp').write_text(probe_cpp)
 cmake=Path('/tmp/shm-contract-perf/CMakeLists.txt').read_text().replace('add_executable(bench_app bench_app.cpp)','add_executable(bench_app bench_app.cpp poll_probe.cpp)\ntarget_link_options(bench_app PRIVATE -Wl,--wrap=poll)')
 (d/'CMakeLists.txt').write_text(cmake)
 if v in ['old_loop','old_loop_hs_off']:shutil.copy2(oldroot/'shm_ipc/event_loop.hpp',d/'include/shm_ipc/event_loop.hpp')
 if v=='old_ring':shutil.copy2(oldroot/'shm_ipc/ringbuf.hpp',d/'include/shm_ipc/ringbuf.hpp')
 if v in ['old_read','direct_dispatch']:
  p=d/'include/shm_ipc/shm_connection.hpp';s=p.read_text();orig=s
  if v=='old_read':
   a=s.index('    int DrainRingOnce()');b=s.index('    /// @brief 启用',a);x=oldconn.index('    int DrainRingOnce()');y=oldconn.index('    /// @brief 启用',x);s=s[:a]+oldconn[x:y]+s[b:]
  else:
   s,n=re.subn(r'CallCodec\(\[&\]\s*\{\s*return\s+([^;]+);\s*\}\)',lambda m:m[1],s);assert n==5,n
   s,n=re.subn(r'CallCallback\(\w+,\s*\[&\]\s*\{\s*([^;]+);\s*\}\)',lambda m:m[1],s);assert n==4,n
  p.write_text(s);(out/(v+'.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/shm_connection.hpp',tofile=v+'/shm_connection.hpp')))
 if v in ['hs_watch_off','old_loop_hs_off']:
  p=d/'include/shm_ipc/shm_server.hpp';s=p.read_text();orig=s;a=s.index('    void OnHandshakeComplete(');b=s.index('    /// @brief 握手成功后的连接建立',a);section=s[a:b];i=section.rfind('\n    }');section=section[:i]+'\n        loop_->RemoveFd(hs_wake_fd_.Get());  // single-client diagnostic only'+section[i:];s=s[:a]+section+s[b:];p.write_text(s)
  (out/(v+'.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/shm_server.hpp',tofile=v+'/shm_server.hpp')))
(out/'prepare.py').write_text(Path('/tmp/prepare-shm-residual.py').read_text())
(out/'poll_probe.hpp').write_text(probe_h);(out/'poll_probe.cpp').write_text(probe_cpp)
(out/'env.txt').write_text(subprocess.check_output(['uname','-a'],text=True)+subprocess.check_output(['c++','--version'],text=True)+subprocess.check_output(['lscpu'],text=True)+'\nRelease -O2 -DNDEBUG; fixed server CPU2/client CPU4. Warmup 1,048,576 replies; measure 8,388,608 replies. Single-client controlled diagnostics only.\n')
def build(v):
 d=str(root/v)
 with (out/(v+'_build.log')).open('w') as f:
  subprocess.run(['cmake','-S',d,'-B',d+'/build','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG'],stdout=f,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['cmake','--build',d+'/build','-j2'],stdout=f,stderr=subprocess.STDOUT,check=True)
 return v
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 for v in ex.map(build,variants):print('built',v,flush=True)
