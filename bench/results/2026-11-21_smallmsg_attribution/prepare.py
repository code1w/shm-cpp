from pathlib import Path
import shutil,subprocess,concurrent.futures,difflib,hashlib
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_smallmsg_attribution';out.mkdir(exist_ok=True)
root=Path('/tmp/shm-smallmsg-attribution');root.mkdir(exist_ok=True)
variants=['before','current','send_self_off','send_codec_off','decode_codec_scope','callback_snapshot_off','buffer_move_off','combined']
for v in variants:
 d=root/v;d.mkdir(exist_ok=True)
 source=Path('/tmp/shm-ca766b1-vs-current/before/include') if v=='before' else repo/'include'
 shutil.copytree(source,d/'include',dirs_exist_ok=True)
 shutil.copy2('/tmp/shm-ca766b1-vs-current/after/bench/bench_app.cpp',d/'bench_app.cpp')
 shutil.copy2('/tmp/shm-budget-removal/CMakeLists.txt',d/'CMakeLists.txt')
 p=d/'include/shm_ipc/shm_connection.hpp';orig=p.read_text();s=orig
 if v!='before':
  assert 'DrainBudget' not in s
  a=s.index('    bool SendCmd(');b=s.index('    /**\n     * @brief 主动关闭连接',a)
  sender=s[a:b]
  if v in ['send_self_off','combined']:
   sender=sender.replace('        auto self = weak_from_this().lock();  // codec 可以重入 owner 并移除当前连接\n','')
  if v in ['send_codec_off','combined']:
   sender=sender.replace('        auto codec = codec_;','        auto* codec = codec_.get();').replace('return codec_ == codec &&','return codec_.get() == codec &&')
  if v in ['buffer_move_off','combined']:
   sender=sender.replace('        auto working_buffer = std::move(encode_buf_);','        auto& working_buffer = encode_buf_;')
  s=s[:a]+sender+s[b:]
  if v in ['decode_codec_scope','combined']:
   s=s.replace('uint32_t off = 0;\n            for (;;)', 'uint32_t off = 0;\n            auto codec = codec_;\n            for (;;)')
   s=s.replace('                auto codec = codec_;\n                if (!codec)', '                if (codec_ != codec) codec = codec_;\n                if (!codec)')
  p.write_text(s)
 if v not in ['before','current']:
  (out/(v+'_connection.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/shm_connection.hpp',tofile=v+'/shm_connection.hpp')))
 if v in ['callback_snapshot_off','combined']:
  p=d/'include/shm_ipc/common.hpp';orig=p.read_text();s=orig.replace('        auto active = callback_;','        const auto& active = callback_;');assert s!=orig;p.write_text(s)
  (out/(v+'_callback.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/common.hpp',tofile=v+'/common.hpp')))
for v in variants:
 assert (root/v/'bench_app.cpp').read_bytes()==(root/'current/bench_app.cpp').read_bytes()
(out/'current_sources.sha256').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(root/'current')) for p in sorted((root/'current').rglob('*')) if p.is_file() and 'build' not in p.parts)+'\n')
(out/'env.txt').write_text(subprocess.check_output(['uname','-a'],text=True)+subprocess.check_output(['c++','--version'],text=True)+subprocess.check_output(['lscpu'],text=True)+'\nCurrent: working tree without reading budget; Before: ca766b1. Release -O2 -DNDEBUG; server CPU2/client CPU4; five rotated/reversed repetitions per configuration.\n')
(out/'prepare.py').write_text(Path('/tmp/prepare-shm-smallmsg-attribution.py').read_text())
def build(v):
 d=str(root/v)
 with (out/(v+'_build.log')).open('w') as f:
  subprocess.run(['cmake','-S',d,'-B',d+'/build','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG'],stdout=f,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['cmake','--build',d+'/build','-j2'],stdout=f,stderr=subprocess.STDOUT,check=True)
 return v
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 for v in ex.map(build,variants):print('built',v,flush=True)
