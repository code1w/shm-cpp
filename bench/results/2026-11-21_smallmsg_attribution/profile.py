#!/usr/bin/env python3
import pathlib,subprocess,csv,re,statistics,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;root=pathlib.Path('/tmp/shm-smallmsg-attribution')
rows=[]
for rnd in range(1,4):
 for batch in [64,256]:
  order=['before','current','combined'] if rnd%2 else ['combined','current','before']
  for variant in order:
   counters=out/f'perf_b{batch}_r{rnd}_{variant}.csv';log=out/f'perf_b{batch}_r{rnd}_{variant}.log'
   with log.open('w') as f:
    subprocess.run(['perf','stat','-x',';','-e','cycles:u,instructions:u','-o',str(counters),'--',str(root/variant/'build/bench_app'),str(batch)],stdout=f,stderr=subprocess.STDOUT,check=True,timeout=90)
   values={}
   for line in counters.read_text().splitlines():
    fields=line.split(';')
    if len(fields)>=3 and fields[0].strip().isdigit():values[fields[2]]=int(fields[0])
   assert 'cycles:u' in values and 'instructions:u' in values,(counters,values)
   rows.append([rnd,batch,variant,values['cycles:u']/1000000,values['instructions:u']/1000000])
   print(rnd,batch,variant,'profiled',flush=True)
with (out/'cpu_per_request.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','batch','variant','user_cycles_per_request','user_instructions_per_request']);w.writerows(rows)
# Separate sampling run; not included in throughput medians.
with (out/'perf_record_current.log').open('w') as f:
 subprocess.run(['perf','record','-q','-e','cycles:u','-F','999','--call-graph','dwarf,4096','-o',str(out/'current.perf.data'),'--',str(root/'current/build/bench_app'),'256'],stdout=f,stderr=subprocess.STDOUT,check=True,timeout=90)
with (out/'perf_report_current.txt').open('w') as f:
 subprocess.run(['perf','report','--stdio','--no-children','--sort=symbol','-i',str(out/'current.perf.data')],stdout=f,stderr=subprocess.STDOUT,check=True)
print('CPU profiling complete',flush=True)
