from pathlib import Path
import shutil,re,subprocess,concurrent.futures,difflib
root=Path('/tmp/shm-smallmsg-attribution');out=Path('/home/dev/github/shm-cpp/bench/results/2026-11-21_smallmsg_attribution')
old=(root/'before/include/shm_ipc/shm_connection.hpp').read_text()
def sender(s):
 a=s.index('    bool SendCmd(');b=s.index('    /**\n     * @brief 主动关闭连接',a);return a,b

def drain(s):
 a=s.index('    int DrainRing()');b=s.index('\n private:',a);return a,b

def once(s):
 a=s.index('    int DrainRingOnce()');b=s.index('    /// @brief 启用',a);return a,b

def replace_section(s,extract):
 a,b=extract(s);x,y=extract(old);return s[:a]+old[x:y]+s[b:]
variants=['legacy_sender','legacy_reader','legacy_callback_storage','legacy_sender_reader','legacy_all_hot','legacy_connection']
for v in variants:
 d=root/v;d.mkdir(exist_ok=True)
 shutil.copytree(root/'current/include',d/'include',dirs_exist_ok=True)
 for file in ['bench_app.cpp','CMakeLists.txt']:shutil.copy2(root/'current'/file,d/file)
 p=d/'include/shm_ipc/shm_connection.hpp';orig=p.read_text();s=orig
 if v=='legacy_connection':s=old
 else:
  if v in ['legacy_sender','legacy_sender_reader','legacy_all_hot']:
   s=replace_section(s,sender)
   x=old.index('    bool GrowEncodeBuffer(');y=old.index('    /// @brief 原始字节写入环',x)
   marker='    // 普通编码与心跳共享借用规则';i=s.index(marker);s=s[:i]+old[x:y]+s[i:]
  if v in ['legacy_reader','legacy_sender_reader','legacy_all_hot']:
   s=replace_section(s,drain);s=replace_section(s,once)
  if v in ['legacy_callback_storage','legacy_all_hot']:
   s,n=re.subn(r'detail::StableCallback<(comm::\w+)>\s+(\w+_)',r'\1 \2',s)
   assert n==4,n
 p.write_text(s)
 (out/(v+'.patch')).write_text(''.join(difflib.unified_diff(orig.splitlines(True),s.splitlines(True),fromfile='current/shm_connection.hpp',tofile=v+'/shm_connection.hpp')))
def build(v):
 d=str(root/v)
 with (out/(v+'_build.log')).open('w') as f:
  subprocess.run(['cmake','-S',d,'-B',d+'/build','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG'],stdout=f,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['cmake','--build',d+'/build','-j2'],stdout=f,stderr=subprocess.STDOUT,check=True)
 return v
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 for v in ex.map(build,variants):print('built',v,flush=True)
(out/'prepare_layers.py').write_text(Path('/tmp/shm-smallmsg-layer-probes.py').read_text())
s=(out/'run.py').read_text()
s=s.replace("variants=['before','current','send_self_off','send_codec_off','decode_codec_scope','callback_snapshot_off','buffer_move_off','combined']","variants=['before','current','legacy_sender','legacy_reader','legacy_callback_storage','legacy_sender_reader','legacy_all_hot','legacy_connection']").replace('range(1,6)','range(1,4)').replace("out/'samples.csv'","out/'samples_layers.csv'").replace("f'b{batch}_r{rnd}_{v}.log'","f'layers_b{batch}_r{rnd}_{v}.log'")
(out/'run_layers.py').write_text(s)
