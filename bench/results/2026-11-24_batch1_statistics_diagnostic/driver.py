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
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('output');p.add_argument('--smoke',action='store_true');p.add_argument('--expanded',action='store_true');p.add_argument('--batch1',action='store_true');p.add_argument('--server-cpu',type=int,default=2);p.add_argument('--client-cpu',type=int,default=4);p.add_argument('--transport',choices=['shm','socket','both'],default='both');a=p.parse_args()
if a.batch1:a.expanded=True
binary=str(Path(a.binary).resolve());out=Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
results=[];children=[];handles=[]
def records(file):return [json.loads(x) for x in file.read_text().splitlines(keepends=True) if x.startswith('{') and x.endswith('\n')]
def launch(role,path,label,flags):
 log=(out/(label+'.jsonl')).open('w');err=(out/(label+'.stderr')).open('w');handles.extend([log,err])
 command=['taskset','-c',str(a.server_cpu if role=='server' else a.client_cpu),binary,role,path]+list(map(str,flags))
 process=subprocess.Popen(command,stdout=log,stderr=err);children.append(process);process.file=out/(label+'.jsonl')
 (out/(label+'.command.json')).write_text(json.dumps(command)+'\n');return process
cases=[('small1',16,1,0,1),('small64',16,64,0,64),('small256',16,256,0,64),('big4k64',4096,64,0,16),('huge1m1',1048576,1,0,1),('huge1m4',1048576,4,0,1),('burst_small1',16,1,1000,1)]
if a.expanded:
 sizes=[16,128,256,512,1024,2048,3072,4096,8192,16384,32768,65536,131072,524288,1048576]
 cases=[]
 for size in sizes:
  label=str(size)+'B'
  batch=64 if size<=32768 else 32 if size<=65536 else 16 if size<=131072 else 4
  if size==16:
   cases.extend([('16B-batch1',16,1,0,1),('16B-batch64',16,64,0,64),('16B-batch256',16,256,0,64)])
  elif size==1048576:
   cases.extend([('1048576B-batch1',size,1,0,1),('1048576B-batch4',size,4,0,1)])
  else:
   stride=64 if size<=512 else 16 if size<=4096 else 1
   cases.append((label+'-batch'+str(batch),size,batch,0,stride))
if a.batch1:
 cases=[(str(size)+'B-batch1',size,1,0,1) for size in sizes]
if a.server_cpu==a.client_cpu:raise ValueError('Choose distinct CPU cores')
cases=[('16B-full',16,1,0,1),('16B-sparse',16,1,0,64)]
try:
 for repeat in range(1 if a.smoke else 3):
  order=cases if repeat%2==0 else list(reversed(cases))
  for index,(name,size,batch,gap,stride) in enumerate(order):
   sides=['shm','socket'] if (repeat+index)%2==0 else ['socket','shm']
   if a.transport!='both':sides=[a.transport]
   for position,side in enumerate(sides):
    directory=Path(tempfile.mkdtemp(prefix='shm-socket-ab-'));path=str(directory/'ipc.sock');label=f'{name}-r{repeat+1}-{side}'
    server=launch('server',path,label+'-server',['--transport',side]+(['--bytes',size,'--batch',batch,'--single-type',1] if a.expanded else []));deadline=time.monotonic()+8
    while not any(x['role']=='ready' for x in records(server.file)):
     assert server.poll() is None and time.monotonic()<deadline;time.sleep(.02)
    client=launch('client',path,label+'-client',['--transport',side]+(['--single-type',1] if a.expanded else [])+['--bytes',size,'--batch',batch,'--gap-us',gap,'--stride',stride,'--warmup',0 if a.smoke else 1,'--seconds',.3 if a.smoke else 5])
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
 (out/'completed.json').write_text(json.dumps({'completed':True,'smoke':a.smoke,'records':len(results),'socket_coalesced':True,'expanded':a.expanded,'batch1':a.batch1,'server_cpu':a.server_cpu,'client_cpu':a.client_cpu,'single_type_codec':a.expanded})+'\n')
finally:
 for child in children:
  if child.poll() is None:child.kill();child.wait()
 for handle in handles:handle.close()
 if 'directory' in globals() and directory.exists():
  if Path(path).exists():Path(path).unlink()
  directory.rmdir()
