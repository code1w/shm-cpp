#!/usr/bin/env python3
import csv,os,pathlib,re,resource,signal,subprocess,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
root=pathlib.Path('/tmp/shm-commit-ab')
CASES=[('small',1),('small',8),('small',64),('small',256),('big4k',8),('big4k',64),('big4k',256),('huge1m',1),('huge1m',2),('huge1m',4)]
pattern=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
def launch(cmd,path):
 log=path.open('w');proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 return proc,log
def finish(proc,log,timeout=90):
 try:
  code=proc.wait(timeout=timeout)
  if code:raise RuntimeError(f'exit={code} command={proc.args} log={log.name}')
 finally:
  if proc.poll() is None:
   os.killpg(proc.pid,signal.SIGKILL);proc.wait()
  log.close()
def throughput(side,rnd):
 path=pathlib.Path('/tmp/fd-pass.socket')
 if path.exists():raise RuntimeError('Existing benchmark socket path: '+str(path))
 server,log=launch(['taskset','-c','2',str(root/side/'build/server'),'--bench'],raw/f'throughput_r{rnd}_{side}_server.log')
 try:
  time.sleep(.3)
  client,clog=launch(['taskset','-c','4',str(root/side/'build/client'),'--bench'],raw/f'throughput_r{rnd}_{side}_client.log')
  finish(client,clog);finish(server,log)
 finally:
  if server.poll() is None:os.killpg(server.pid,signal.SIGKILL);server.wait()
  log.close()
 rows=[]
 for line in (raw/f'throughput_r{rnd}_{side}_client.log').read_text().splitlines():
  t=line.split()
  if len(t)==6 and t[0].isdigit():rows.append([t[0],t[1],t[3],t[4]])
 assert len(rows)==19,rows
 return rows

def echo(side,rnd):
 path=raw/f'echo_r{rnd}_{side}.log';p,f=launch([str(root/side/'build/bench_shm')],path);finish(p,f)
 rows=[]
 for line in path.read_text().splitlines():
  t=line.split()
  if len(t)==5 and t[0].isdigit():rows.append([t[0],t[1],t[3],t[4]])
 assert len(rows)==11,rows
 return rows

def app(side,rnd,kind,batch):
 args=[str(root/side/'build/bench_app'),str(batch)]
 if kind!='small':args.append('--big' if kind=='big4k' else '--huge')
 path=raw/f'app_{kind}_b{batch}_r{rnd}_{side}.log';p,f=launch(args,path);finish(p,f)
 m=re.search(pattern,path.read_text());assert m,path
 return m.groups()

with (out/'throughput.csv').open('w') as tf,(out/'rtt.csv').open('w') as ef,(out/'app.csv').open('w') as af:
 tw=csv.writer(tf);ew=csv.writer(ef);aw=csv.writer(af)
 tw.writerow(['round','position','side','payload','messages','msg_s','mib_s'])
 ew.writerow(['round','position','side','payload','messages','mean_rtt_ns','echo_mib_s'])
 aw.writerow(['round','position','side','kind','batch','messages','seconds','msg_s','mib_s'])
 for rnd in range(1,6):
  for group_index,(name,fn,writer,file) in enumerate([('throughput',throughput,tw,tf),('rtt',echo,ew,ef)]):
   order=['before','after'] if (rnd+group_index)%2 else ['after','before']
   for pos,side in enumerate(order,1):
    for sample in fn(side,rnd):writer.writerow([rnd,pos,side,*sample])
    file.flush()
   print('round',rnd,name,'/'.join(order),'passed',flush=True)
  for index,(kind,batch) in enumerate(CASES):
   order=['before','after'] if (rnd+index)%2 else ['after','before'];rates={}
   for pos,side in enumerate(order,1):
    values=app(side,rnd,kind,batch);aw.writerow([rnd,pos,side,*values]);af.flush();rates[side]=int(values[4])
   print(f'round={rnd} app={kind} batch={batch} order={"/".join(order)} before={rates["before"]} after={rates["after"]} change={(rates["after"]/rates["before"]-1)*100:+.1f}%',flush=True)
