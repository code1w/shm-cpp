#!/usr/bin/env python3
"""Full-sample serial application RTT check after (not during) the soak."""
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
binary=str(Path(sys.argv[1]).resolve());out=Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=True)
dir=Path(tempfile.mkdtemp(prefix='shm-rtt-'));path=str(dir/'ipc.sock');processes=[];handles=[]
def launch(role,label,flags=()):
    log=(out/(label+'.jsonl')).open('w');err=(out/(label+'.stderr')).open('w');handles.extend([log,err])
    command=['taskset','-c','2' if role=='server' else '4',binary,role,path]+list(map(str,flags))
    p=subprocess.Popen(command,stdout=log,stderr=err);processes.append(p)
    (out/(label+'.command.json')).write_text(json.dumps(command)+'\n');return p
try:
    server=launch('server','server');deadline=time.monotonic()+8
    while not any('"ready"' in x for x in (out/'server.jsonl').read_text().splitlines()):
        assert server.poll() is None and time.monotonic()<deadline;time.sleep(.02)
    client=launch('client','client',['--seconds',10,'--warmup',1,'--batch',1,'--stride',1])
    assert client.wait(timeout=25)==0
    records=[json.loads(x) for x in (out/'client.jsonl').read_text().splitlines() if x.startswith('{')]
    result=[x for x in records if x['role']=='result'][0];final=records[-1]
    assert result['samples']==result['messages'] and result['samples']>=100000 and final['completed'] and not final['failed'] and final['base_fds']==final['final_fds'] and final['ring_maps']==0
    server.send_signal(signal.SIGTERM);assert server.wait(timeout=5)==0
    server_records=[json.loads(x) for x in (out/'server.jsonl').read_text().splitlines() if x.startswith('{')]
    assert server_records[-1]['role']=='server_final' and server_records[-1]['opened']==server_records[-1]['closed']
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
finally:
    for p in processes:
        if p.poll() is None:p.kill();p.wait()
    for handle in handles:handle.close()
    if Path(path).exists():Path(path).unlink()
    dir.rmdir()
