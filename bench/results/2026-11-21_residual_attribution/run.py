#!/usr/bin/env python3
import pathlib,csv,subprocess,re,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
root=pathlib.Path('/tmp/shm-residual-attribution')
variants=['baseline','current','old_loop','old_ring','old_read','direct_dispatch','hs_watch_off','old_loop_hs_off']
pat=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','batch','position','variant','measured_messages','seconds','msg_s','mib_s','server_polls','client_polls'])
 for rnd in range(1,4):
  for ci,batch in enumerate([64,256]):
   start=(rnd+ci)%len(variants);order=variants[start:]+variants[:start]
   if (rnd+ci)%2:order=list(reversed(order))
   for pos,v in enumerate(order,1):
    path=raw/f'b{batch}_r{rnd}_{v}.log'
    with path.open('w') as log:r=subprocess.run([str(root/v/'build/bench_app'),str(batch)],stdout=log,stderr=subprocess.STDOUT,timeout=120)
    assert r.returncode==0,(path,r.returncode)
    text=path.read_text();m=re.search(pat,text);assert m and int(m[3])==8388608,(path,text[-500:])
    counts={role:int(value) for role,value in re.findall(r'probe role=(server|client) poll_calls=(\d+)',text)}
    assert set(counts)=={'server','client'},(path,counts)
    w.writerow([rnd,batch,pos,v,*m.groups()[2:],counts['server'],counts['client']]);f.flush()
    print('round',rnd,'batch',batch,v,'rate',m[5],'polls',counts,flush=True)
