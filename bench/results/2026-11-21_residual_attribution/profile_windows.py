#!/usr/bin/env python3
import pathlib,subprocess,csv,re,time,os,signal,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent
paths={('short','baseline'):'/tmp/shm-ca766b1-vs-current/before/build/bench_app',('short','current'):'/tmp/shm-contract-perf/build/bench_app',('warm','baseline'):'/tmp/shm-residual-attribution/baseline/build/bench_app',('warm','current'):'/tmp/shm-residual-attribution/current/build/bench_app'}
def prime():
 ps=[subprocess.Popen(['taskset','-c',str(cpu),'python3','-c','while True: pass'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True) for cpu in [2,4]]
 try:time.sleep(.4)
 finally:
  for p in ps:os.killpg(p.pid,signal.SIGKILL)
  for p in ps:p.wait()
rows=[]
for rnd in range(1,4):
 for batch in [64,256]:
  modes=['short','primed','warm'] if rnd%2 else ['warm','primed','short']
  for mode in modes:
   sides=['baseline','current'] if (rnd+batch//64)%2 else ['current','baseline']
   for side in sides:
    if mode=='primed':prime()
    binary=paths[('warm' if mode=='warm' else 'short',side)]
    stat=out/f'window_{mode}_b{batch}_r{rnd}_{side}.stat';log=out/f'window_{mode}_b{batch}_r{rnd}_{side}.log'
    with log.open('w') as f:
     subprocess.run(['perf','stat','-e','cycles:u,instructions:u','-o',str(stat),'--',binary,str(batch)],stdout=f,stderr=subprocess.STDOUT,check=True,timeout=120)
    text=stat.read_text();output=log.read_text()
    rate=re.search(r'rate=\s*(\d+) msg/s',output);assert rate,log
    cyc=re.search(r'([\d,]+)\s+cycles:u',text);ins=re.search(r'([\d,]+)\s+instructions:u',text);user=re.search(r'([\d.]+) seconds user',text);assert cyc and ins and user,stat
    cycles=int(cyc[1].replace(',',''));instructions=int(ins[1].replace(',',''));utime=float(user[1]);total=9437184 if mode=='warm' else 1000000
    rows.append([rnd,batch,mode,side,int(rate[1]),cycles/total,instructions/total,utime,cycles/utime/1e9])
    print(rnd,batch,mode,side,'rate',rate[1],'GHz',round(cycles/utime/1e9,2),flush=True)
with (out/'window_profiles.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','batch','window','side','msg_s','user_cycles_per_total_reply','user_instructions_per_total_reply','user_cpu_seconds','effective_user_GHz']);w.writerows(rows)
