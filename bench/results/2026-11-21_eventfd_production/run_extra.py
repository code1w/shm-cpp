#!/usr/bin/env python3
import os, pathlib, subprocess, time
root = pathlib.Path(__file__).resolve().parent
build = root.parents[2] / 'build'

def run(name,args):
    start=time.monotonic()
    cmd=['taskset','-c','2,4',str(build/args[0]),*args[1:]]
    with (root/name).open('w') as log:
        r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    print(name,'exit',r.returncode,'seconds',round(time.monotonic()-start,2),flush=True)
    if r.returncode: raise SystemExit(r.returncode)

for rep in range(1,4):
    run(f'bench_shm_run{rep}.txt',['bench_shm'])
    run(f'bench_socket_run{rep}.txt',['bench_socket'])
for rep in range(1,4):
    for kind,batches in [('small',[1,8,64,256]),('big4k',[8,64,256]),('huge1m',[1,2,4])]:
        for batch in batches:
            args=['bench_app',str(batch)]
            if kind!='small':args.append('--big' if kind=='big4k' else '--huge')
            run(f'app_{kind}_b{batch}_run{rep}.txt',args)
run('bench_codec.txt',['bench_codec'])
run('bench_batch.txt',['bench_batch'])
