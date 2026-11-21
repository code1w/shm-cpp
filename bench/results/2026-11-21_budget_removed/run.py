#!/usr/bin/env python3
import pathlib,csv,subprocess,re,resource,statistics
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
paths={'before':'/tmp/shm-regression-probes/current/build/bench_app','after':'/tmp/shm-budget-removal/build/bench_app'}
pat=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','kind','batch','position','side','messages','seconds','msg_s','mib_s'])
 for rnd in range(1,4):
  for ci,(kind,batch) in enumerate([('small',64),('small',256),('huge1m',4)]):
   order=['before','after'] if (rnd+ci)%2 else ['after','before']
   for pos,side in enumerate(order,1):
    args=[paths[side],str(batch)]
    if kind=='huge1m':args.append('--huge')
    path=raw/f'{kind}_b{batch}_r{rnd}_{side}.log'
    with path.open('w') as log:r=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=90)
    assert r.returncode==0,(path,r.returncode)
    m=re.search(pat,path.read_text());assert m,path
    w.writerow([rnd,kind,batch,pos,side,*m.groups()[2:]]);f.flush()
   print(rnd,kind,batch,'passed',flush=True)
rs=list(csv.DictReader((out/'samples.csv').open()));assert len(rs)==18
text=['# 删除读取预算后的针对性复测','',
'Before 为 76d4c83 的带预算实现；After 为删除预算后的工作区。使用相同 bench_app 和固定线程绑核（server CPU2/client CPU4），Release -O2 -DNDEBUG，3对交错测试。此次只删除读取预算、时钟检查和预算自唤醒，所有生命周期/重入保护保留。', '',
'以下吞吐列为各侧三次中位数；百分比为逐对 after/before−1 的中位数，不与两列直接相除混同。MiB/s按程序使用的1024²字节计算。', '',
'| 场景 | 删除前 msg/s | 删除后 msg/s | 配对变化 | 删除后 MiB/s |','|---|---:|---:|---:|---:|']
for kind,batch in [('small','64'),('small','256'),('huge1m','4')]:
 b=[r for r in rs if r['side']=='before' and r['kind']==kind and r['batch']==batch];a=[r for r in rs if r['side']=='after' and r['kind']==kind and r['batch']==batch]
 ds=[(int(next(r for r in a if r['round']==br['round'])['msg_s'])/int(br['msg_s'])-1)*100 for br in b]
 text.append(f'| {kind}/batch{batch} | {statistics.median(int(r["msg_s"]) for r in b):,.0f} | {statistics.median(int(r["msg_s"]) for r in a):,.0f} | {statistics.median(ds):+.1f}% | {statistics.median(float(r["mib_s"]) for r in a):.1f} |')
text+=['', '这只是3档/3对的针对性复测，不代表全部吞吐与尾延迟验收。Debug 14项测试通过；ASan/UBSan含泄漏检查14项均通过（并发测试首次共用固定socket路径导致test_shm_app启动冲突，隔离重跑通过）。生产 build/ Release 产物已重建。', '',
'回归覆盖：单次读取超过64帧/2ms、多个1MiB帧、读取期间到达的新消息、连续心跳、读空后无自动自唤醒；原有协议错误、EOF、异常与重入保护测试继续通过。', '',
'当前使用前提：同机、少连接、可信对端、消息量受控。预算移除未删除共享内存边界校验、编码缓冲保活、codec/连接/回调保活及reader_wait按需通知。']
(out/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
