#!/usr/bin/env python3
"""Paired application comparison: current SHM/eventfd vs coalesced UDS.
Usage: compare_socket.py BINARY OUTPUT [--smoke]
"""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import time
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('output');p.add_argument('--smoke',action='store_true');p.add_argument('--transport',choices=['shm','socket','both'],default='both');a=p.parse_args()
binary=str(Path(a.binary).resolve());out=Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
results=[];children=[];handles=[]
def records(file):return [json.loads(x) for x in file.read_text().splitlines(keepends=True) if x.startswith('{') and x.endswith('\n')]
def launch(role,path,label,flags):
 log=(out/(label+'.jsonl')).open('w');err=(out/(label+'.stderr')).open('w');handles.extend([log,err])
 command=['taskset','-c','2' if role=='server' else '4',binary,role,path]+list(map(str,flags))
 process=subprocess.Popen(command,stdout=log,stderr=err);children.append(process);process.file=out/(label+'.jsonl')
 (out/(label+'.command.json')).write_text(json.dumps(command)+'\n');return process
cases=[('small1',16,1,0,1),('small64',16,64,0,64),('small256',16,256,0,64),('big4k64',4096,64,0,16),('huge1m1',1048576,1,0,1),('huge1m4',1048576,4,0,1),('burst_small1',16,1,1000,1)]
try:
 for repeat in range(1 if a.smoke else 3):
  order=cases if repeat%2==0 else list(reversed(cases))
  for index,(name,size,batch,gap,stride) in enumerate(order):
   sides=['shm','socket'] if (repeat+index)%2==0 else ['socket','shm']
   if a.transport!='both':sides=[a.transport]
   for position,side in enumerate(sides):
    directory=Path(tempfile.mkdtemp(prefix='shm-socket-ab-'));path=str(directory/'ipc.sock');label=f'{name}-r{repeat+1}-{side}'
    server=launch('server',path,label+'-server',['--transport',side]);deadline=time.monotonic()+8
    while not any(x['role']=='ready' for x in records(server.file)):
     assert server.poll() is None and time.monotonic()<deadline;time.sleep(.02)
    client=launch('client',path,label+'-client',['--transport',side,'--bytes',size,'--batch',batch,'--gap-us',gap,'--stride',stride,'--warmup',0 if a.smoke else 1,'--seconds',.3 if a.smoke else 10])
    code=client.wait(timeout=25);assert code==0,(label,code)
    data=records(client.file);result=[x for x in data if x['role']=='result'][0];final=data[-1]
    assert final['role']=='client_final' and final['completed'] and not final['failed'] and final['base_fds']==final['final_fds'] and final['ring_maps']==0
    assert result['messages']>0 and result['samples']>0 and result['errors']==0
    server.send_signal(signal.SIGTERM);assert server.wait(timeout=5)==0
    serverdata=records(server.file);assert serverdata[-1]['role']=='server_final' and serverdata[-1]['opened']==serverdata[-1]['closed'] and serverdata[-1]['base_fds']==serverdata[-1]['final_fds']
    assert all(x.get('send_failures',0)==0 for x in serverdata)
    results.append(dict(result,side=side,case=name,round=repeat+1,position=position))
    (out/'samples.json').write_text(json.dumps(results,indent=2)+'\n')
    if Path(path).exists():Path(path).unlink()
    directory.rmdir()
    print(label,'RPC/s',round(result['msg_s']),'P99.9 us',result['p999_ns']/1000,flush=True)
 (out/'completed.json').write_text(json.dumps({'completed':True,'smoke':a.smoke,'records':len(results),'socket_coalesced':True})+'\n')
finally:
 for child in children:
  if child.poll() is None:child.kill();child.wait()
 for handle in handles:handle.close()
 if 'directory' in globals() and directory.exists():
  if Path(path).exists():Path(path).unlink()
  directory.rmdir()
