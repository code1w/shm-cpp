from pathlib import Path
import subprocess,tarfile,io,hashlib,difflib,shutil
repo=Path('/home/dev/github/shm-cpp');out=repo/'bench/results/2026-11-21_ca766b1_vs_current';out.mkdir(exist_ok=True)
root=Path('/tmp/shm-ca766b1-vs-current');root.mkdir(exist_ok=True)
paths=['include','src','proto','bench/bench_app.cpp','bench/bench_shm.cpp']
for side in ['before','after']:
 src=root/side;src.mkdir(exist_ok=True)
 if side=='before':
  data=subprocess.check_output(['git','archive','ca766b1',*paths],cwd=repo)
  with tarfile.open(fileobj=io.BytesIO(data)) as tar:tar.extractall(src)
 else:
  for rel in paths:
   p=repo/rel
   if p.is_dir():shutil.copytree(p,src/rel,dirs_exist_ok=True)
   else:(src/rel).parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,src/rel)
 # 两侧应用相同的既有绑核补丁，只固定server/client线程和进程到CPU2/4。
 for name,patch in [('bench_shm.cpp','pin_echo.patch'),('bench_app.cpp','pin_app.patch')]:
  original=(src/'bench'/name).read_text()
  pinned=out/(name+'.pinned.cpp')
  # 先验证原始基准与此前测试版本一致，再复用相同的绑核源码。
  expected=subprocess.check_output(['git','show','ca766b1:bench/'+name],text=True,cwd=repo)
  assert original==expected,name
  (src/'bench'/name).write_text(pinned.read_text())
  if side=='after':(out/patch).write_text(''.join(difflib.unified_diff(original.splitlines(True),pinned.read_text().splitlines(True),fromfile='bench/'+name,tofile='bench/'+name)))
 shutil.copy2(out/'CMakeLists.bench.txt',src/'CMakeLists.txt')
 manifest=[]
 for p in sorted(src.rglob('*')):
  if p.is_file() and 'build' not in p.parts:manifest.append(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(src)))
 (out/f'{side}_sources.sha256').write_text('\n'.join(manifest)+'\n')
for rel in ['bench/bench_app.cpp','bench/bench_shm.cpp','src/server.cpp','src/client.cpp','CMakeLists.txt']:
 assert (root/'before'/rel).read_bytes()==(root/'after'/rel).read_bytes(),rel
(out/'current_vs_ca766b1.patch').write_bytes(subprocess.check_output(['git','diff','--binary','ca766b1','--',*paths],cwd=repo))
(out/'env.txt').write_text('\n'.join(subprocess.check_output(cmd,text=True,cwd=repo) for cmd in [['uname','-a'],['c++','--version'],['lscpu'],['git','rev-parse','ca766b1'],['git','rev-parse','HEAD'],['git','status','--short']])+'\nBefore: ca766b1; After: working tree, reading budget removed. Release -O2 -DNDEBUG, no sanitizer; server CPU2/client CPU4; five interleaved pairs.\n')
(out/'prepare.py').write_text(Path(__file__).read_text())
