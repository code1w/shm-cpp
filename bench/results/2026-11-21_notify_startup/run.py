#!/usr/bin/env python3
import pathlib,csv,re,subprocess,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
root=pathlib.Path('/tmp/shm-notify-startup')
variants=['baseline','current','arm_early','force_client_always']
pat=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','batch','position','variant','messages','seconds','msg_s','mib_s','client_reader_mode','server_writes','client_writes','server_polls','client_polls'])
 for rnd in range(1,6):
  for ci,batch in enumerate([64,256]):
   start=(rnd+ci)%4;order=variants[start:]+variants[:start]
   if (rnd+ci)%2:order=list(reversed(order))
   for pos,v in enumerate(order,1):
    path=raw/f'b{batch}_r{rnd}_{v}.log'
    with path.open('w') as log:r=subprocess.run([str(root/v/'build/bench_app'),str(batch)],stdout=log,stderr=subprocess.STDOUT,timeout=120)
    assert r.returncode==0,(path,r.returncode)
    text=path.read_text();m=re.search(pat,text);assert m,text[-1000:]
    mode=re.search(r'probe reader_mode=(\d+)',text);assert mode,path
    counts={role:(int(p),int(w)) for role,p,w in re.findall(r'probe role=(server|client) poll_calls=(\d+) notify_writes=(\d+)',text)}
    assert set(counts)=={'server','client'},(path,counts)
    w.writerow([rnd,batch,pos,v,*m.groups()[2:],mode[1],counts['server'][1],counts['client'][1],counts['server'][0],counts['client'][0]]);f.flush()
    print(rnd,batch,v,'rate',m[5],'mode',mode[1],'server_writes',counts['server'][1],flush=True)
