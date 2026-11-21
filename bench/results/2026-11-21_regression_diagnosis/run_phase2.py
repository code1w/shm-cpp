#!/usr/bin/env python3
import csv,pathlib,subprocess,resource,re
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
root=pathlib.Path('/tmp/shm-regression-probes')
variants=['before','current','no_callback_snapshot','no_send_self','no_send_buffer_move']
cases=[('small',64),('small',256),('huge1m',4)]
pat=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples_phase2.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','kind','batch','position','variant','messages','seconds','msg_s','mib_s'])
 for rnd in range(1,4):
  for ci,(kind,batch) in enumerate(cases):
   order=variants[(rnd+ci)%len(variants):]+variants[:(rnd+ci)%len(variants)]
   if (rnd+ci)%2:order=list(reversed(order))
   for pos,v in enumerate(order,1):
    args=[str(root/v/'build/bench_app'),str(batch)]
    if kind=='huge1m':args.append('--huge')
    path=raw/f'phase2_{kind}_b{batch}_r{rnd}_{v}.log'
    with path.open('w') as log:r=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=90)
    assert r.returncode==0,(path,r.returncode)
    m=re.search(pat,path.read_text());assert m,path
    w.writerow([rnd,kind,batch,pos,v,*m.groups()[2:]]);f.flush()
   print('round',rnd,kind,'batch',batch,'completed',flush=True)
