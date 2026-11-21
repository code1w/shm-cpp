from pathlib import Path
import shutil,subprocess,hashlib,difflib
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_regression_diagnosis';out.mkdir(exist_ok=True)
root=Path('/tmp/shm-regression-probes');root.mkdir(exist_ok=True)
variants=['before','current','no_budget','relaxed_caps','no_frame_clock','codec_scope']
for name in variants:
 source=Path('/tmp/shm-commit-ab')/('before' if name=='before' else 'after')
 dest=root/name;dest.mkdir(exist_ok=True)
 shutil.copytree(source/'include',dest/'include',dirs_exist_ok=True)
 shutil.copy2(source/'bench/bench_app.cpp',dest/'bench_app.cpp')
 (dest/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.14)
project(regression_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -pedantic)
include_directories(include)
add_executable(bench_app bench_app.cpp)
target_link_libraries(bench_app PRIVATE Threads::Threads)
''')
 p=dest/'include/shm_ipc/shm_connection.hpp';orig=p.read_text();s=orig
 if name=='no_budget':
  assert 'DrainBudget budget(drain_all);' in s
  s=s.replace('DrainBudget budget(drain_all);','DrainBudget budget(true);')
 if name=='relaxed_caps':
  assert 'frames >= 64 || bytes >= 256 * 1024' in s
  s=s.replace('frames >= 64 || bytes >= 256 * 1024','frames >= 1024 || bytes >= 8 * 1024 * 1024')
 if name=='no_frame_clock':
  assert 'exhausted = frames >= 64 || bytes >= 256 * 1024 ||\n                        std::chrono::steady_clock::now() >= deadline;' in s
  s=s.replace('exhausted = frames >= 64 || bytes >= 256 * 1024 ||\n                        std::chrono::steady_clock::now() >= deadline;','exhausted = frames >= 64 || bytes >= 256 * 1024;')
 if name=='codec_scope':
  assert 'uint32_t off = 0;\n            for (;;)' in s
  s=s.replace('uint32_t off = 0;\n            for (;;)', 'uint32_t off = 0;\n            auto codec = codec_;\n            for (;;)')
  s=s.replace('                auto codec = codec_;\n                if (!codec)', '                if (codec_ != codec) codec = codec_;\n                if (!codec)')
 p.write_text(s)
 if name not in ['before','current']:
  (out/f'{name}.patch').write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/shm_connection.hpp',tofile=name+'/shm_connection.hpp')))
for name in variants:
 assert (root/name/'bench_app.cpp').read_bytes()==(root/'current/bench_app.cpp').read_bytes()
(out/'env.txt').write_text(subprocess.check_output(['uname','-a'],text=True)+subprocess.check_output(['c++','--version'],text=True)+subprocess.check_output(['lscpu'],text=True)+'\nRelease -O2 -DNDEBUG; fixed server CPU2/client CPU4; temporary benchmark variants, no production changes.\n')
(out/'prepare.py').write_text(Path('/tmp/prepare-shm-regression-probes.py').read_text())
