#!/usr/bin/env python3
"""One-case sensitivity check: same binary, full-batch vs 64-message flush.
Builds a TEMP benchmark variant; production headers and base adapter unchanged.
Usage: check_socket_chunking.py OUTPUT
"""
import difflib
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import sys
root=Path(__file__).resolve().parents[1];out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
children=[];handles=[]
with tempfile.TemporaryDirectory(prefix='socket-chunk-profile-') as build:
 build=Path(build)
 header=(root/'bench/socket_peer.hpp').read_text();cpp=(root/'bench/bench_acceptance.cpp').read_text()
 assert header.count('if(!coalesce)Flush();')==1
 new_header=header.replace('inline bool coalesce=true;','inline bool coalesce=true;\ninline size_t flush_threshold=0;').replace('if(!coalesce)Flush();','if(!coalesce || (flush_threshold && tx_size_-tx_off_>=flush_threshold))Flush();')
 new_cpp=cpp.replace('else if(k=="--socket-buffer")','else if(k=="--socket-threshold")socket_bench::flush_threshold=std::strtoull(v,nullptr,10);else if(k=="--socket-buffer")')
 assert cpp!=new_cpp
 (build/'socket_peer.hpp').write_text(new_header);(build/'bench_acceptance.cpp').write_text(new_cpp)
 (out/'socket_peer.patch').write_text(''.join(difflib.unified_diff(header.splitlines(True),new_header.splitlines(True),fromfile='base/socket_peer.hpp',tofile='profile/socket_peer.hpp')))
 (out/'bench_acceptance.patch').write_text(''.join(difflib.unified_diff(cpp.splitlines(True),new_cpp.splitlines(True),fromfile='base/bench_acceptance.cpp',tofile='profile/bench_acceptance.cpp')))
 binary=build/'bench'
 compile_command=['c++','-std=c++17','-O2','-DNDEBUG','-Wall','-Wextra','-pedantic','-I'+str(root/'include'),str(build/'bench_acceptance.cpp'),'-pthread','-o',str(binary)]
 compile_run=subprocess.run(compile_command,capture_output=True,text=True,timeout=120)
 (out/'compile.log').write_text(compile_run.stdout+compile_run.stderr);assert compile_run.returncode==0
 (out/'binary.sha256').write_text(hashlib.sha256(binary.read_bytes()).hexdigest()+'  temporary-profile-binary\n')
 def rows(file):return [json.loads(x) for x in file.read_text().splitlines(keepends=True) if x.startswith('{') and x.endswith('\n')]
 def launch(role,path,label,flags):
  log=(out/(label+'.jsonl')).open('w');err=(out/(label+'.stderr')).open('w');handles.extend([log,err])
  command=['taskset','-c','2' if role=='server' else '4',str(binary),role,path]+list(map(str,flags))
  p=subprocess.Popen(command,stdout=log,stderr=err);p.file=out/(label+'.jsonl');children.append(p)
  (out/(label+'.command.json')).write_text(json.dumps(command)+'\n');return p
 samples=[];directory=None
 try:
  modes=[('shm','shm',0),('socket_full','socket',0),('socket_chunk64','socket',1280)]
  for repeat in range(3):
   order=modes[repeat:]+modes[:repeat]
   for name,transport,threshold in order:
    directory=Path(tempfile.mkdtemp(prefix='socket-chunk-case-'));path=str(directory/'ipc.sock');label=f'{name}-r{repeat+1}'
    flags=['--transport',transport,'--socket-threshold',threshold]
    server=launch('server',path,label+'-server',flags);deadline=time.monotonic()+8
    while not any(x['role']=='ready' for x in rows(server.file)):
     assert server.poll() is None and time.monotonic()<deadline;time.sleep(.02)
    client=launch('client',path,label+'-client',flags+['--seconds',5,'--warmup',1,'--batch',256,'--stride',64])
    assert client.wait(timeout=20)==0
    data=rows(client.file);result=[x for x in data if x['role']=='result'][0];final=data[-1]
    assert final['completed'] and not final['failed'] and final['final_fds']==final['base_fds'] and final['ring_maps']==0 and result['errors']==0
    server.send_signal(signal.SIGTERM);assert server.wait(timeout=5)==0
    sr=rows(server.file);assert sr[-1]['opened']==sr[-1]['closed'] and sr[-1]['base_fds']==sr[-1]['final_fds']
    samples.append(dict(result,mode=name,round=repeat+1,threshold=threshold))
    (out/'samples.json').write_text(json.dumps(samples,indent=2)+'\n')
    if Path(path).exists():Path(path).unlink()
    directory.rmdir();directory=None
    print(name,'r',repeat+1,round(result['msg_s']),'RPC/s',flush=True)
  (out/'completed.json').write_text(json.dumps({'completed':True,'records':9,'case':'16B batch256','measured_seconds_per_run':5,'warmup_seconds':1})+'\n')
 finally:
  for child in children:
   if child.poll() is None:child.kill();child.wait()
  for handle in handles:handle.close()
  if directory is not None:
   if Path(path).exists():Path(path).unlink()
   directory.rmdir()
