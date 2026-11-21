from pathlib import Path
import shutil,difflib,subprocess,concurrent.futures
root=Path('/tmp/shm-regression-probes');out=Path('/home/dev/github/shm-cpp/bench/results/2026-11-21_regression_diagnosis')
variants=['no_callback_snapshot','no_send_self','no_send_buffer_move']
for v in variants:
 d=root/v;d.mkdir(exist_ok=True)
 shutil.copytree(root/'current/include',d/'include',dirs_exist_ok=True)
 for name in ['CMakeLists.txt','bench_app.cpp']:shutil.copy2(root/'current'/name,d/name)
 if v=='no_callback_snapshot':
  p=d/'include/shm_ipc/common.hpp';orig=p.read_text();s=orig.replace('        auto active = callback_;','        const auto& active = callback_;')
 else:
  p=d/'include/shm_ipc/shm_connection.hpp';orig=p.read_text();s=orig
  if v=='no_send_self':s=s.replace('        auto self = weak_from_this().lock();  // codec 可以重入 owner 并移除当前连接','        // Diagnostic only: caller owns the connection in this benchmark.')
  if v=='no_send_buffer_move':s=s.replace('        auto working_buffer = std::move(encode_buf_);\n        EncodeGuard guard(sending_, encode_buf_, working_buffer);','        auto& working_buffer = encode_buf_;\n        EncodeGuard guard(sending_, encode_buf_, working_buffer);',1)
 assert orig!=s,v;p.write_text(s)
 (out/(v+'.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/'+p.name,tofile=v+'/'+p.name)))
def build(v):
 d=str(root/v)
 with (out/(v+'_build.log')).open('w') as f:
  subprocess.run(['cmake','-S',d,'-B',d+'/build','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG'],stdout=f,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['cmake','--build',d+'/build','-j2'],stdout=f,stderr=subprocess.STDOUT,check=True)
 return v
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 for v in ex.map(build,variants):print('built',v,flush=True)
(out/'prepare_phase2.py').write_text(Path('/tmp/shm-regression-phase2.py').read_text())
p=out/'run_phase2.py';s=(out/'run.py').read_text().replace("variants=['before','current','no_budget','relaxed_caps','no_frame_clock','codec_scope']","variants=['before','current','no_callback_snapshot','no_send_self','no_send_buffer_move']").replace("out/'samples.csv'","out/'samples_phase2.csv'").replace("(rnd+ci)%6", "(rnd+ci)%len(variants)")
s=s.replace("f'{kind}_b{batch}_r{rnd}_{v}.log'","f'phase2_{kind}_b{batch}_r{rnd}_{v}.log'");p.write_text(s)
