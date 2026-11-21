#!/usr/bin/env python3
import csv,pathlib,re,resource,subprocess,statistics
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
out=pathlib.Path(__file__).resolve().parent;raw=out/'raw';raw.mkdir(exist_ok=True)
paths={'baseline':'/tmp/shm-ca766b1-vs-current/before/build/bench_app','before':'/tmp/shm-ca766b1-vs-current/after/build/bench_app','after':'/tmp/shm-contract-perf/build/bench_app'}
variants=list(paths)
pat=r'type=(\w+)\s+batch=\s*(\d+) total=(\d+) time=\s*([\d.]+)s\s+rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
with (out/'samples.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['round','kind','batch','position','side','messages','seconds','msg_s','mib_s'])
 for rnd in range(1,6):
  for ci,(kind,batch) in enumerate([('small',64),('small',256),('huge1m',4)]):
   start=(rnd+ci)%3;order=variants[start:]+variants[:start]
   if (rnd+ci)%2:order=list(reversed(order))
   for pos,side in enumerate(order,1):
    args=[paths[side],str(batch)]
    if kind=='huge1m':args.append('--huge')
    path=raw/f'{kind}_b{batch}_r{rnd}_{side}.log'
    with path.open('w') as log:r=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=90)
    assert r.returncode==0,(path,r.returncode)
    m=re.search(pat,path.read_text());assert m,path
    w.writerow([rnd,kind,batch,pos,side,*m.groups()[2:]]);f.flush()
   print(rnd,kind,batch,'all variants passed',flush=True)
rs=list(csv.DictReader((out/'samples.csv').open()));assert len(rs)==45
text=['# 单线程codec契约与热路径修复验证','',
'修复：codec虚函数仅做编解码，不重入连接管理/收发、不修改codec或连接缓冲；配置在收发操作之外变更，执行中的连接回调不能自替换。调用方保持连接/codec有效。ShmConnection的消息回调仍可正常SendCmd回复和Close，owner已有的局部强引用保证收尾。Debug用assert检查常见违约；Release不为任意重入复制保活快照、不移动编码缓冲所有权，也不更新Debug保护标志。',
'',
'源码保留错误返回、扩容重试、协议/长度/游标校验、异常关闭/提交、验证定时器和正常连接生命周期管理。所有Debug与ASan/UBSan含泄漏检查各14项测试通过；新增/调整测试覆盖25种codec/回调违约断言、合法编解码扩容与普通消息回复/关闭。生产build/ Release产物已重建。',
'',
'性能：baseline为ca766b1；before为此前删除预算后、尚有逐包保活的工作区快照；after为本次修复。三侧相同bench_app、Release -O2 -DNDEBUG、server CPU2/client CPU4。每档五次，顺序旋转反转，共45组。下面百分比是同轮after/before以及after/baseline逐对变化的中位数，吞吐列分别是五次中位数。',
'',
'| 场景 | ca766b1 msg/s | 修复前 msg/s | 修复后 msg/s | 对修复前配对变化 | 对基线配对变化 |','|---|---:|---:|---:|---:|---:|']
for kind,batch in [('small','64'),('small','256'),('huge1m','4')]:
 sets={side:[r for r in rs if r['kind']==kind and r['batch']==batch and r['side']==side] for side in variants}
 med={side:statistics.median(int(r['msg_s']) for r in data) for side,data in sets.items()}
 deltas={}
 for side in ['before','baseline']:
  deltas[side]=[(int(next(r for r in sets['after'] if r['round']==b['round'])['msg_s'])/int(b['msg_s'])-1)*100 for b in sets[side]]
 text.append(f'| {kind}/batch{batch} | {med["baseline"]:,.0f} | {med["before"]:,.0f} | {med["after"]:,.0f} | {statistics.median(deltas["before"]):+.1f}% | {statistics.median(deltas["baseline"]):+.1f}% |')
text+=['','| 场景 | ca766b1样本范围 | 修复前范围 | 修复后范围 |','|---|---:|---:|---:|']
for kind,batch in [('small','64'),('small','256'),('huge1m','4')]:
 cells=[kind+'/batch'+batch]
 for side in variants:
  vals=[int(r['msg_s']) for r in rs if r['kind']==kind and r['batch']==batch and r['side']==side];cells.append(f'{min(vals):,}–{max(vals):,}')
 text.append('| '+' | '.join(cells)+' |')
text+=['','这些是3档的针对性验证，不是全部载荷/多连接尾延迟测试。非法重入在Release属于契约之外的行为，不能把Debug断言理解为Release也提供隔离。低频的server/client工厂和生命周期事务保护继续保留。', '',
'原始输出在raw/，数据在samples.csv，源码身份在source.sha256。implementation.patch为当前实现相对HEAD的源码差异，包含此前删除预算和本次契约简化；尚未提交或推送。']
(out/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
