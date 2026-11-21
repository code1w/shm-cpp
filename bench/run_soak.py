#!/usr/bin/env python3
"""SHM soak supervisor. Streams logs/samples; memory does not grow with duration.
Start: run_soak.py BINARY OUTPUT --seconds 604800
Inspect/stop: run_soak.py --status OUTPUT / --stop OUTPUT
"""
import argparse
import csv
from datetime import datetime,timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

p=argparse.ArgumentParser()
p.add_argument('binary',nargs='?');p.add_argument('output',nargs='?')
p.add_argument('--seconds',type=int,default=604800);p.add_argument('--warmup',type=int,default=5)
p.add_argument('--bytes',type=int,choices=[16,4096,1048576],default=16);p.add_argument('--batch',type=int,default=256)
p.add_argument('--sample-interval',type=float,default=10);p.add_argument('--stall-timeout',type=float,default=60)
p.add_argument('--server-cpu',type=int,default=2);p.add_argument('--client-cpu',type=int,default=4)
p.add_argument('--status',type=Path);p.add_argument('--stop',type=Path)
a=p.parse_args()
def identity(pid):
    # Fields after the final ')' start at stat field 3; starttime is field 22.
    stat=Path('/proc',str(pid),'stat').read_text();return stat.rsplit(')',1)[1].split()[19]
def tail_records(file):
    if not file.exists():return []
    with file.open('rb') as f:
        f.seek(0,2);size=f.tell();f.seek(max(0,size-16384));data=f.read()
    if size>16384:data=data.split(b'\n',1)[-1]
    return [json.loads(x) for x in data.splitlines(keepends=True) if x.startswith(b'{') and x.endswith(b'\n')]
def atomic_json(file,data):
    temp=file.with_suffix(file.suffix+'.tmp');temp.write_text(json.dumps(data,indent=2)+'\n');temp.replace(file)
if a.status or a.stop:
    directory=(a.status or a.stop).resolve();status=json.loads((directory/'status.json').read_text())
    owner=json.loads((directory/'pids.json').read_text())
    if a.status:
        try:alive=identity(owner['runner_pid'])==owner['runner_starttime'] and Path('/proc/sys/kernel/random/boot_id').read_text().strip()==owner['boot_id']
        except FileNotFoundError:alive=False
        status['runner_alive']=alive
        if status['state']=='running' and not alive:status['state']='runner_absent'
        print(json.dumps(status,indent=2));raise SystemExit(0)
    if status['state']!='running':raise SystemExit('Test is already '+status['state'])
    try:valid=identity(owner['runner_pid'])==owner['runner_starttime'] and Path('/proc/sys/kernel/random/boot_id').read_text().strip()==owner['boot_id']
    except FileNotFoundError:valid=False
    if not valid:raise SystemExit('Runner is absent or PID was reused; no signal sent')
    os.kill(owner['runner_pid'],signal.SIGTERM);print('Stop requested; inspect status.json for final cleanup');raise SystemExit(0)
if not a.binary or not a.output:p.error('BINARY and OUTPUT are required')
if a.seconds<=0 or a.warmup<0 or a.sample_interval<=0 or a.stall_timeout<=0:p.error('Invalid timing')
if a.batch<1 or a.batch>256 or (a.bytes==1048576 and a.batch>4):p.error('Invalid batch for payload size')
if a.server_cpu==a.client_cpu:p.error('Use distinct CPU cores')
binary=Path(a.binary).resolve()
if not binary.is_file():p.error('Binary does not exist')
out=Path(a.output).resolve();out.mkdir(parents=True,exist_ok=False)
socket_dir=Path(tempfile.mkdtemp(prefix='shm-soak-'));socket_path=str(socket_dir/'ipc.sock')
children=[];handles=[];interrupted=False;forced=[];server=None;client=None
start=time.monotonic();last_progress=start;last_replies=0;state='running';reason=None
status={'state':state,'started_utc':datetime.now(timezone.utc).isoformat(),'seconds_requested':a.seconds,'warmup_seconds':a.warmup,'output':str(out),'bytes':a.bytes,'batch':a.batch,'server_cpu':a.server_cpu,'client_cpu':a.client_cpu}
def stopped(signum,frame):
    global interrupted
    interrupted=True
signal.signal(signal.SIGTERM,stopped);signal.signal(signal.SIGINT,stopped)
owner={'runner_pid':os.getpid(),'runner_starttime':identity(os.getpid()),'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
atomic_json(out/'pids.json',owner);atomic_json(out/'status.json',status)
(out/'binary.sha256').write_text(hashlib.sha256(binary.read_bytes()).hexdigest()+'  '+str(binary)+'\n')
def launch(role):
    log=(out/(role+'.jsonl')).open('w');err=(out/(role+'.stderr')).open('w');handles.extend([log,err])
    cmd=['taskset','-c',str(a.server_cpu if role=='server' else a.client_cpu),str(binary),role,socket_path]
    if role=='client':cmd+=['--seconds',str(a.seconds),'--warmup',str(a.warmup),'--bytes',str(a.bytes),'--batch',str(a.batch),'--stride','64']
    child=subprocess.Popen(cmd,stdout=log,stderr=err,start_new_session=True);child.role=role;children.append(child)
    owner[role+'_pid']=child.pid;owner[role+'_starttime']=identity(child.pid);atomic_json(out/'pids.json',owner)
    (out/(role+'.command.json')).write_text(json.dumps(cmd)+'\n');return child
fields=['elapsed_s','role','pid','rss_kib','anon_kib','shmem_kib','fds','ring_maps','threads']
resources=(out/'resources.csv').open('w',newline='');writer=csv.DictWriter(resources,fieldnames=fields);writer.writeheader();handles.append(resources)
def sample(child,elapsed):
    try:
        values={}
        for line in Path('/proc',str(child.pid),'status').read_text().splitlines():
            key,_,v=line.partition(':');values[key]=v.strip()
        maps=Path('/proc',str(child.pid),'maps').read_text().splitlines()
        value={'elapsed_s':round(elapsed,3),'role':child.role,'pid':child.pid,
            'fds':len(list(Path('/proc',str(child.pid),'fd').iterdir())),
            'ring_maps':sum('memfd:client_to_server' in x or 'memfd:server_to_client' in x for x in maps),'threads':int(values['Threads'])}
        for key,source in [('rss_kib','VmRSS'),('anon_kib','RssAnon'),('shmem_kib','RssShmem')]:
            value[key]=int(values[source].split()[0]) if source in values else None
        writer.writerow(value);resources.flush()
    except (FileNotFoundError,ProcessLookupError):pass
    except PermissionError:
        time.sleep(.01)
        if child.poll() is None:raise

def finish_child(child):
    if child is None or child.poll() is not None:return
    child.send_signal(signal.SIGTERM)
    try:child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        forced.append(child.role);os.killpg(child.pid,signal.SIGKILL);child.wait()
try:
    server=launch('server');deadline=time.monotonic()+8
    while not any(x['role']=='ready' for x in tail_records(out/'server.jsonl')):
        if interrupted:raise InterruptedError('stop requested')
        if server.poll() is not None:raise RuntimeError('server startup failed')
        if time.monotonic()>deadline:raise TimeoutError('server startup timeout')
        time.sleep(.02)
    client=launch('client');last_progress=time.monotonic();next_sample=0;next_report=0
    while client.poll() is None:
        now=time.monotonic();elapsed=now-start
        if interrupted:raise InterruptedError('stop requested')
        if server.poll() is not None:raise RuntimeError('server exited during load')
        for row in tail_records(out/'client.jsonl'):
            if row['role']=='client_tick' and row['replies']>last_replies:
                last_replies=row['replies'];last_progress=now
        if now-last_progress>a.stall_timeout:raise TimeoutError('no reply progress')
        if elapsed>a.seconds+a.warmup+30:raise TimeoutError('overall timeout')
        if elapsed>=next_sample:
            sample(server,elapsed);sample(client,elapsed);next_sample=elapsed+a.sample_interval
            status.update(elapsed_s=round(elapsed,1),latest_replies=last_replies,progress_age_s=round(now-last_progress,1))
            atomic_json(out/'status.json',status)
        if elapsed>=next_report:
            status.update(elapsed_s=round(elapsed,1),latest_replies=last_replies,progress_age_s=round(now-last_progress,1))
            atomic_json(out/'status.json',status);print(json.dumps(status),flush=True);next_report=elapsed+60
        time.sleep(.25)
    if interrupted:raise InterruptedError('stop requested')
    if client.returncode!=0:raise RuntimeError('client exited with code '+str(client.returncode))
    rows=tail_records(out/'client.jsonl');result=next(x for x in rows if x['role']=='result');final=rows[-1]
    if not final['completed'] or final['failed'] or final['base_fds']!=final['final_fds'] or final['ring_maps']!=0:raise RuntimeError('client completion/cleanup failed')
    atomic_json(out/'result.json',result)
    status.update(latest_replies=final['received'],measured_replies=result['messages'])
    # Wait for the server to observe EOF; it remains alive for final cleanup.
    deadline=time.monotonic()+5
    while True:
        ticks=[x for x in tail_records(out/'server.jsonl') if x['role']=='server_tick']
        if ticks and ticks[-1]['active']==0 and ticks[-1]['ring_maps']==0:break
        if server.poll() is not None or time.monotonic()>deadline:raise RuntimeError('server did not clean up connection')
        time.sleep(.05)
    finish_child(server)
    sf=tail_records(out/'server.jsonl')[-1]
    if server.returncode!=0 or sf['role']!='server_final' or sf['base_fds']!=sf['final_fds'] or sf['opened']!=sf['closed']:raise RuntimeError('server completion/cleanup failed')
    state='passed'
except InterruptedError as error:state='interrupted';reason=str(error)
except Exception as error:state='failed';reason=repr(error)
finally:
    finish_child(client);finish_child(server)
    for handle in handles:handle.close()
    if Path(socket_path).exists():Path(socket_path).unlink()
    socket_dir.rmdir()
    status.update(state=state,reason=reason,elapsed_s=round(time.monotonic()-start,3),finished_utc=datetime.now(timezone.utc).isoformat(),forced_kills=forced,
                  exit_codes={c.role:c.returncode for c in children})
    atomic_json(out/'status.json',status);print(json.dumps(status),flush=True)
raise SystemExit(0 if state=='passed' else 130 if state=='interrupted' else 1)
