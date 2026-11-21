#!/usr/bin/env python3
import csv, pathlib, re, subprocess, time, resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent
raw=out/'raw';raw.mkdir(exist_ok=True)
root=pathlib.Path('/tmp/shm-notify-ab')
cases=[('small',1),('small',8),('small',64),('small',256),('big4k',8),('big4k',64),('big4k',256),('huge1m',1),('huge1m',2),('huge1m',4)]
pattern=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples.csv').open('w') as file:
 writer=csv.writer(file);writer.writerow(['round','kind','batch','position','backend','total','seconds','msg_s','mib_s'])
 for round in range(1,6):
  for index,(kind,batch) in enumerate(cases):
   order=['eventfd','socketpair'] if (round+index)%2 else ['socketpair','eventfd']
   rates={}
   for pos,backend in enumerate(order,1):
    args=[str(root/backend/'build/bench_app'),str(batch)]
    if kind!='small':args.append('--big' if kind=='big4k' else '--huge')
    log=raw/f'{kind}_b{batch}_r{round}_{backend}.txt'
    with log.open('w') as f:
     r=subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,timeout=90)
    if r.returncode:raise RuntimeError(f'{log}: exit {r.returncode}')
    m=re.search(pattern,log.read_text());assert m,log
    assert m[1]==kind and int(m[2])==batch
    writer.writerow([round,kind,batch,pos,backend,*m.groups()[2:]]);file.flush()
    rates[backend]=int(m[5])
   print(f'round={round} {kind} batch={batch} order={"/".join(order)} eventfd={rates["eventfd"]} socketpair={rates["socketpair"]} socket/event={rates["socketpair"]/rates["eventfd"]:.3f}',flush=True)
