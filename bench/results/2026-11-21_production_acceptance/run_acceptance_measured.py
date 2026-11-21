#!/usr/bin/env python3
"""Bounded production acceptance: app echo, resource soak, exit/reconnect.
Usage: run_acceptance.py BINARY OUTPUT [--smoke] [--soak-seconds 600]
"""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import tempfile
import time

parser=argparse.ArgumentParser();parser.add_argument('binary');parser.add_argument('output');parser.add_argument('--smoke',action='store_true');parser.add_argument('--skip-performance',action='store_true');parser.add_argument('--soak-seconds',type=int,default=600)
a=parser.parse_args();binary=str(Path(a.binary).resolve());out=Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
socket_dir=Path(tempfile.mkdtemp(prefix='shm-accept-'));path=str(socket_dir/'ipc.sock')
children=[];metrics=[];resources=[];faults=[]
def rows(file):
    if not file.exists():return []
    return [json.loads(line) for line in file.read_text().splitlines(keepends=True) if line.startswith('{') and line.endswith('\n')]
def launch(role,label,flags=()):
    file=out/(label+'.jsonl');log=file.open('w');err=(out/(label+'.stderr')).open('w')
    command=['taskset','-c','2' if role=='server' else '4',binary,role,path]+list(map(str,flags))
    p=subprocess.Popen(command,stdout=log,stderr=err);children.append((p,log,err));p.file=file;p.role=role
    (out/(label+'.command.json')).write_text(json.dumps(command)+'\n')
    return p
def wait_ready(p):
    deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        if any(x['role']=='ready' for x in rows(p.file)):return
        if p.poll() is not None:raise RuntimeError('server startup failed')
        time.sleep(.02)
    raise TimeoutError('server ready')
def sample(p,phase,elapsed):
    try:
        status={}
        for line in Path('/proc',str(p.pid),'status').read_text().splitlines():
            key,_,value=line.partition(':');status[key]=value.strip()
        maps=Path('/proc',str(p.pid),'maps').read_text().splitlines()
        resources.append({'phase':phase,'elapsed':round(elapsed,3),'pid':p.pid,'role':p.role,
            'fds':len(list(Path('/proc',str(p.pid),'fd').iterdir())),
            'ring_maps':sum('memfd:client_to_server' in x or 'memfd:server_to_client' in x for x in maps),
            'rss_kib':int(status.get('VmRSS','0 kB').split()[0]),'anon_kib':int(status.get('RssAnon','0 kB').split()[0]),
            'shmem_kib':int(status.get('RssShmem','0 kB').split()[0]),'threads':int(status.get('Threads','0'))})
    except (FileNotFoundError,ProcessLookupError):pass
    except PermissionError:
        # Linux can deny /proc/PID/fd while the same-UID process is exiting.
        # Do not swallow a permission failure on a still-running target.
        time.sleep(.01)
        if p.poll() is None:raise

def wait_client(p,timeout,phase,server=None):
    begin=time.monotonic();last=-1;progress=0
    while p.poll() is None:
        elapsed=time.monotonic()-begin
        if elapsed>timeout:raise TimeoutError(phase)
        if int(elapsed)!=last:
            sample(p,phase,elapsed)
            if server:sample(server,phase,elapsed)
            last=int(elapsed)
        if elapsed>=progress+30:print(phase,'elapsed',round(elapsed),'s',flush=True);progress=elapsed
        time.sleep(.03)
    return p.returncode

def stop_server(p):
    p.send_signal(signal.SIGTERM);assert p.wait(timeout=5)==0
    final=[x for x in rows(p.file) if x['role']=='server_final'][-1]
    assert final['opened']==final['closed'] and final['base_fds']==final['final_fds'] and final['ring_maps']==0

def wait_idle(p):
    start=time.monotonic();seen=len([x for x in rows(p.file) if x['role']=='server_tick'])
    while time.monotonic()-start<5:
        ticks=[x for x in rows(p.file) if x['role']=='server_tick']
        if len(ticks)>seen and ticks[-1]['active']==0 and ticks[-1]['ring_maps']==0:return ticks[-1]
        time.sleep(.05)
    raise RuntimeError('server did not reclaim connection')

def validate(p,label):
    data=rows(p.file);final=[x for x in data if x['role']=='client_final'][-1]
    assert final['completed'] and not final['failed'] and final['base_fds']==final['final_fds'] and final['ring_maps']==0
    values=[x for x in data if x['role']=='result'];assert values
    for result in values:assert result['errors']==0 and result['samples']>0
    metrics.extend(dict(x,case=label) for x in values)
    return final
try:
    # Performance measurements are serial and do not overlap the soak/chaos.
    if a.skip_performance:
        for file in sorted(out.glob('*-r[123].jsonl')):
            data=rows(file);final=[x for x in data if x['role']=='client_final']
            assert len(final)==1 and final[0]['completed'] and not final[0]['failed'] and final[0]['base_fds']==final[0]['final_fds'] and final[0]['ring_maps']==0
            values=[x for x in data if x['role']=='result'];assert len(values)==1 and values[0]['errors']==0
            metrics.append(dict(values[0],case=file.stem))
        assert len(metrics)==21
        print('Reused 21 complete validated performance records',flush=True)
    else:
        server=launch('server','perf-server');wait_ready(server)
        cases=[('small1',16,1,0,64),('small64',16,64,0,64),('small256',16,256,0,64),('big4k64',4096,64,0,16),('huge1m1',1048576,1,0,1),('huge1m4',1048576,4,0,1),('burst_small1',16,1,1000,1)]
        repeats=1 if a.smoke else 3;duration=.25 if a.smoke else 10;warm=0 if a.smoke else 1
        for repeat in range(repeats):
            order=cases if repeat%2==0 else list(reversed(cases))
            for name,size,batch,gap,stride in order:
                label=f'{name}-r{repeat+1}'
                client=launch('client',label,['--bytes',size,'--batch',batch,'--gap-us',gap,'--stride',stride,'--seconds',duration,'--warmup',warm])
                assert wait_client(client,duration+warm+10,label,server)==0;validate(client,label)
                print('performance',label,'passed',flush=True)
        idle=wait_idle(server);assert idle['send_failures']==0;stop_server(server)
    # Normal reconnects reuse the SAME client object and event loop.
    server=launch('server','fault-server');wait_ready(server)
    sessions=8 if a.smoke else 300
    client=launch('client','reconnect-same-client',['--sessions',sessions,'--messages',256,'--warmup',0,'--stride',16,'--batch',64])
    assert wait_client(client,60,'reconnect',server)==0
    final=validate(client,'reconnect');assert final['sessions']==sessions and final['close_events']==sessions
    idle=wait_idle(server);assert idle['opened']==sessions and idle['closed']==sessions
    faults.append({'scenario':'normal_same_client_reconnect','sessions':sessions,'passed':True})
    # Kill an active client, keep the server alive, verify cleanup then recover.
    for repeat in range(1 if a.smoke else 5):
        client=launch('client',f'kill-client-{repeat}',['--seconds',60,'--warmup',0,'--batch',64])
        deadline=time.monotonic()+5
        while not any(x['role']=='client_connected' for x in rows(client.file)):
            assert client.poll() is None and time.monotonic()<deadline;time.sleep(.02)
        time.sleep(.3);assert client.poll() is None
        before=time.monotonic();kill_ns=time.monotonic_ns();client.kill();assert client.wait(timeout=5)==-signal.SIGKILL
        idle=wait_idle(server)
        recovered=launch('client',f'after-client-kill-{repeat}',['--messages',256,'--warmup',0,'--stride',1])
        assert wait_client(recovered,10,'client-kill-recovery',server)==0;validate(recovered,'client-kill-recovery');wait_idle(server)
        faults.append({'scenario':'client_sigkill','round':repeat,'cleanup_and_recovery_test_s':time.monotonic()-before,'close_callback_ms':([x for x in rows(server.file) if x['role']=='server_close'][-1]['time_ns']-kill_ns)/1e6,'passed':True})
    stop_server(server)
    # Kill/restart server while a SAME client object retries in its loop timer.
    for repeat in range(1 if a.smoke else 3):
        server=launch('server',f'kill-server-{repeat}');wait_ready(server)
        client=launch('client',f'survive-server-kill-{repeat}',['--seconds',.4 if a.smoke else 3,'--warmup',0,'--sessions',2,'--reconnect',1,'--stride',16])
        deadline=time.monotonic()+5
        while not any(x['role']=='client_connected' for x in rows(client.file)):
            assert client.poll() is None and time.monotonic()<deadline;time.sleep(.02)
        time.sleep(.15 if a.smoke else .5);assert client.poll() is None
        owned=os.lstat(path)
        kill_ns=time.monotonic_ns();server.kill();assert server.wait(timeout=5)==-signal.SIGKILL
        start=time.monotonic()
        refused=launch('server',f'stale-path-refusal-{repeat}')
        assert refused.wait(timeout=5)==3
        current=os.lstat(path)
        assert stat.S_ISSOCK(current.st_mode) and (current.st_dev,current.st_ino)==(owned.st_dev,owned.st_ino)
        Path(path).unlink() # own isolated node, owner confirmed dead
        server=launch('server',f'restarted-server-{repeat}');wait_ready(server)
        assert wait_client(client,15,'server-restart',server)==0
        final=validate(client,'server-restart');assert final['sessions']==2 and final['close_events']==2
        faults.append({'scenario':'server_sigkill_same_client_recovery','round':repeat,'recovery_test_s':time.monotonic()-start,'client_disconnect_ms':([x for x in rows(client.file) if x['role']=='client_close' and x['unexpected']][0]['time_ns']-kill_ns)/1e6,'client_reconnected_ms':([x for x in rows(client.file) if x['role']=='client_connected' and x['session']==2][0]['time_ns']-kill_ns)/1e6,'stale_path_direct_restart_refused':True,'supervisor_cleanup_required':True,'passed':True})
        wait_idle(server);stop_server(server)
    # Persistent two-process load; sample resources without touching hot paths.
    server=launch('server','soak-server');wait_ready(server)
    seconds=2 if a.smoke else a.soak_seconds
    client=launch('client','soak-client',['--seconds',seconds,'--warmup',0 if a.smoke else 5,'--batch',256,'--stride',64])
    assert wait_client(client,seconds+20,'soak',server)==0;validate(client,'soak');wait_idle(server);stop_server(server)
    (out/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    (out/'faults.json').write_text(json.dumps(faults,indent=2)+'\n')
    with (out/'resources.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=list(resources[0]));writer.writeheader();writer.writerows(resources)
    (out/'run.json').write_text(json.dumps({'completed':True,'smoke':a.smoke,'soak_seconds':seconds,'synthetic_app_load':True,'performance_reused':a.skip_performance,'server_cpu':2,'client_cpu':4,'cases':len(metrics),'faults':len(faults)},indent=2)+'\n')
    print('ALL ACCEPTANCE SCENARIOS PASSED',flush=True)
finally:
    for p,log,err in children:
        if p.poll() is None:p.kill();p.wait()
        log.close();err.close()
    if Path(path).exists():Path(path).unlink()
    socket_dir.rmdir()
