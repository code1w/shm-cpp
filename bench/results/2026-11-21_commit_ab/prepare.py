from pathlib import Path
import subprocess,tarfile,io,hashlib,difflib
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_commit_ab';out.mkdir(exist_ok=True)
root=Path('/tmp/shm-commit-ab');root.mkdir(exist_ok=True)
refs={'before':'ca766b1','after':'76d4c83'}
for side,ref in refs.items():
 src=root/side;src.mkdir(exist_ok=True)
 data=subprocess.check_output(['git','archive',ref,'include','src','bench/bench_app.cpp','bench/bench_shm.cpp','proto'],cwd=repo)
 with tarfile.open(fileobj=io.BytesIO(data)) as tar:tar.extractall(src)
 # 相同基准源码增加相同绑核，分别固定 server CPU2 / client CPU4。
 p=src/'bench/bench_shm.cpp';orig=p.read_text();s=orig.replace('#include <sys/socket.h>','#include <sys/socket.h>\n#include <sched.h>')
 helper='''void PinCpu(int cpu)
{
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(cpu, &cpus);
    if (::sched_setaffinity(0, sizeof(cpus), &cpus) != 0) std::abort();
}

'''
 s=s.replace('using Channel = shm::DefaultRingChannel;',helper+'using Channel = shm::DefaultRingChannel;')
 s=s.replace('void RunServer(int socket_fd)\n{','void RunServer(int socket_fd)\n{\n    PinCpu(2);').replace('void RunClient(int socket_fd)\n{','void RunClient(int socket_fd)\n{\n    PinCpu(4);')
 p.write_text(s)
 if side=='after':(out/'pin_echo.patch').write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='bench/bench_shm.cpp',tofile='bench/bench_shm.cpp')))
 p=src/'bench/bench_app.cpp';orig=p.read_text();s=orig.replace('#include <thread>','#include <thread>\n#include <pthread.h>\n#include <sched.h>')
 s=s.replace('std::thread server_thread([&] { (void)server_loop.Run(50); });','''std::thread server_thread([&] {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(2, &cpus);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus)) std::abort();
        (void)server_loop.Run(50);
    });''')
 s=s.replace('std::thread client_thread([&] {','''std::thread client_thread([&] {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(4, &cpus);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus)) std::abort();''')
 p.write_text(s)
 if side=='after':(out/'pin_app.patch').write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='bench/bench_app.cpp',tofile='bench/bench_app.cpp')))
 (src/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.14)
project(shm_commit_ab LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall -Wextra -pedantic)
find_package(Threads REQUIRED)
include_directories(include)
list(APPEND CMAKE_PREFIX_PATH /home/dev/github/shm-cpp/3rd/protobuf /home/dev/github/shm-cpp/3rd/protobuf/lib64/cmake/protobuf)
find_package(Protobuf REQUIRED)
add_library(shm_proto proto/shm_messages.pb.cc)
target_include_directories(shm_proto PUBLIC proto ${Protobuf_INCLUDE_DIRS})
target_link_libraries(shm_proto PUBLIC protobuf::libprotobuf)
add_executable(server src/server.cpp)
add_executable(client src/client.cpp)
add_executable(bench_shm bench/bench_shm.cpp)
add_executable(bench_app bench/bench_app.cpp)
foreach(t server client bench_shm bench_app)
 target_link_libraries(${t} PRIVATE Threads::Threads)
endforeach()
target_link_libraries(server PRIVATE shm_proto)
target_link_libraries(client PRIVATE shm_proto)
target_compile_definitions(server PRIVATE SHM_IPC_HAS_PROTOBUF)
target_compile_definitions(client PRIVATE SHM_IPC_HAS_PROTOBUF)
''')
 manifest=[]
 for p in sorted(src.rglob('*')):
  if p.is_file() and 'build' not in p.parts:
   manifest.append(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(src)))
 (out/f'{side}_sources.sha256').write_text('\n'.join(manifest)+'\n')
for rel in ['bench/bench_app.cpp','bench/bench_shm.cpp','src/server.cpp','src/client.cpp','CMakeLists.txt']:
 assert (root/'before'/rel).read_bytes()==(root/'after'/rel).read_bytes(),rel
(out/'env.txt').write_text('\n'.join(subprocess.check_output(cmd,text=True,cwd=repo) for cmd in [['uname','-a'],['c++','--version'],['lscpu'],['git','rev-parse','ca766b1'],['git','rev-parse','76d4c83']])+'\nRelease -O2 -DNDEBUG; no sanitizer; server CPU2/client CPU4; five interleaved pairs; identical harness sources.\n')
(out/'prepare.py').write_text(Path('/tmp/prepare-shm-commit-ab.py').read_text())
