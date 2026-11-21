from pathlib import Path
import shutil,hashlib,difflib,subprocess
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_notify_ab';out.mkdir(parents=True,exist_ok=True)
root=Path('/tmp/shm-notify-ab');root.mkdir(exist_ok=True)
for variant in ['socketpair','eventfd']:
 src=root/variant;src.mkdir(exist_ok=True)
 shutil.copytree(repo/'include',src/'include',dirs_exist_ok=True)
 shutil.copytree(repo/'test',src/'test',dirs_exist_ok=True)
 shutil.copy2(repo/'bench/bench_app.cpp',src/'bench_app.cpp')
 app=(src/'bench_app.cpp').read_text()
 app=app.replace('#include <thread>','#include <thread>\n#include <pthread.h>\n#include <sched.h>')
 app=app.replace('std::thread server_thread([&] { (void)server_loop.Run(50); });','''std::thread server_thread([&] {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(2, &cpus);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus)) std::abort();
        (void)server_loop.Run(50);
    });''')
 app=app.replace('std::thread client_thread([&] {','''std::thread client_thread([&] {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(4, &cpus);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus)) std::abort();''')
 (src/'bench_app.cpp').write_text(app)
 (src/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.14)
project(notify_ab LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -pedantic)
include_directories(include)
add_executable(bench_app bench_app.cpp)
target_link_libraries(bench_app PRIVATE Threads::Threads)
foreach(t test_shm_app test_ringbuf_boundary test_audit_round3_fixes)
 add_executable(${t} test/${t}.cpp)
 target_link_libraries(${t} PRIVATE Threads::Threads)
 target_compile_options(${t} PRIVATE -UNDEBUG)
endforeach()
target_link_options(test_audit_round3_fixes PRIVATE -Wl,--wrap=listen)
''')
 if variant=='eventfd':
  p=src/'include/shm_ipc/ring_channel.hpp';s=p.read_text()
  s=s.replace('::send(notify_write_fd_.Get(), &v, sizeof(v), MSG_DONTWAIT | MSG_NOSIGNAL)','::write(notify_write_fd_.Get(), &v, sizeof(v))')
  s=s.replace('::send(notify_local_write_fd_.Get(), &one, sizeof(one), MSG_DONTWAIT | MSG_NOSIGNAL)','::write(notify_local_write_fd_.Get(), &one, sizeof(one))')
  a=s.index('        uint64_t count = 0;',s.index('    static uint64_t DrainNotify'));b=s.index('\n    /// @brief 单次写入最大字节数',a)
  s=s[:a]+'''        uint64_t value = 0;
        ssize_t n;
        do { n = ::read(fd, &value, sizeof(value)); } while (n < 0 && errno == EINTR);
        return n == sizeof(value) ? value : 0;
    }
''' +s[b:]
  a=s.index('        int fds[2];',s.index('    ErrorCode CreateNotifyPair'));b=s.index('\n    /// @brief 创建写环',a)
  s=s[:a]+'''        ErrorCode ec = CreateEventfd(&notify_read_fd_, sys_errno);
        if (ec != ErrorCode::kOk) return ec;
        const int fd = ::fcntl(notify_read_fd_.Get(), F_DUPFD_CLOEXEC, 0);
        if (fd < 0) return ErrnoFailure(errno, sys_errno);
        notify_local_write_fd_ = UniqueFd{fd};
        return ErrorCode::kOk;
    }
''' +s[b:]
  s=s.replace('FdTag::kNotifySocket','FdTag::kEventfd').replace('ValidateNotifySocket(notify_write_fd_.Get(), sys_errno)','ValidateNotifyEventfd(notify_write_fd_.Get(), sys_errno)')
  p.write_text(s)
# archive only experimental differences and source identity, no production edits
socket=(root/'socketpair/include/shm_ipc/ring_channel.hpp').read_text().splitlines(True)
event=(root/'eventfd/include/shm_ipc/ring_channel.hpp').read_text().splitlines(True)
(out/'eventfd.patch').write_text(''.join(difflib.unified_diff(socket,event,fromfile='socketpair/include/shm_ipc/ring_channel.hpp',tofile='eventfd/include/shm_ipc/ring_channel.hpp')))
orig=(repo/'bench/bench_app.cpp').read_text().splitlines(True);pinned=(root/'socketpair/bench_app.cpp').read_text().splitlines(True)
(out/'pin_threads.patch').write_text(''.join(difflib.unified_diff(orig,pinned,fromfile='bench/bench_app.cpp',tofile='bench_app.cpp')))
manifest=[]
for p in sorted((root/'socketpair').rglob('*')):
 if p.is_file():
  rel=p.relative_to(root/'socketpair');q=root/'eventfd'/rel
  if rel!=Path('include/shm_ipc/ring_channel.hpp'):assert p.read_bytes()==q.read_bytes(),rel
  manifest.append(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {rel}')
(out/'socketpair_sources.sha256').write_text('\n'.join(manifest)+'\n')
(out/'prepare.py').write_text(Path('/tmp/prepare-shm-notify-ab.py').read_text())
(out/'env.txt').write_text(subprocess.check_output(['uname','-a'],text=True)+subprocess.check_output(['c++','--version'],text=True)+subprocess.check_output(['lscpu'],text=True)+'\nRelease -O2 -DNDEBUG; fixed server thread CPU2/client thread CPU4; 5 paired runs per case.\n')
