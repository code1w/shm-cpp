#!/usr/bin/env python3
import pathlib,csv,subprocess,re,resource,statistics
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
root=pathlib.Path('/tmp/shm-smallmsg-attribution')
variants=['before','current','legacy_sender','legacy_reader','legacy_callback_storage','legacy_sender_reader','legacy_all_hot','legacy_connection']
pat=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples_layers.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','batch','position','variant','messages','seconds','msg_s','mib_s'])
 for rnd in range(1,4):
  for ci,batch in enumerate([64,256]):
   start=(rnd+ci)%len(variants);order=variants[start:]+variants[:start]
   if (rnd+ci)%2:order=list(reversed(order))
   for pos,v in enumerate(order,1):
    path=raw/f'layers_b{batch}_r{rnd}_{v}.log'
    with path.open('w') as log:r=subprocess.run([str(root/v/'build/bench_app'),str(batch)],stdout=log,stderr=subprocess.STDOUT,timeout=90)
    assert r.returncode==0,(path,r.returncode)
    m=re.search(pat,path.read_text());assert m,path
    w.writerow([rnd,batch,pos,v,*m.groups()[2:]]);f.flush()
   print('round',rnd,'batch',batch,'all variants passed',flush=True)
