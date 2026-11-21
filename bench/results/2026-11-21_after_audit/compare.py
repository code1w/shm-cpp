#!/usr/bin/env python3
import pathlib,re,statistics
root=pathlib.Path(__file__).resolve().parent
hist=root.parent/'2026-11-10_o2_after_p15'
def throughput(path):
 d={}
 for line in path.read_text().splitlines():
  t=line.split()
  if len(t)==6 and re.fullmatch(r'[0-9.]+(?:B|KB|MB)',t[0]):
   try:d[t[0]]=[float(v) for v in t[1:5]]
   except ValueError:pass
 return d
old=throughput(hist/'bench_sh.txt');new=throughput(root/'bench_sh.txt')
text=['# 审核修复后性能对比（2026-11-21）','',
'当前工作区 Release `-O2 -DNDEBUG`，无 sanitizer；服务器 CPU 2、客户端 CPU 4。吞吐基准每档 3 次取中位数，原始日志保留在 `raw/`。主对照为 2026-11-10_o2_after_p15，同优化级别、同吞吐脚本与核号。MB/s 沿用程序标签，实际按 1024² 字节计算。',
'', '## 主要观察', '', '端到端单向吞吐多数档位相对历史为 -1.4% 到 +4.5%；64B 中位数下降 7.0%，但本轮 1634.8–1892.7 MB/s 的范围包含历史 1764.5 MB/s，不能仅据三次测量认定稳定退化。上层 small/batch256 从旧 deferred 的 6,039,311 降至 3,385,292 msg/s（-43.9%），small/batch1 从旧 deferred 的 95,168 降至 71,194（-25.2%），需要关注小消息路径；big4k/batch64 相对旧 immediate 为 +27.2%，huge1m/batch4 相对旧 immediate 为 -17.5%。这些是跨版本、跨通知模式的观测差异，缺少同代码路径的即时 A/B，不能归因到某一处修复。', '', '全部 44 次程序运行退出 0：6 次端到端吞吐、6 次 echo、30 次上层回声、2 个内部 best-of-3 微基准。原始数据、环境和复跑脚本已归档。', '', '## 端到端单向吞吐', '', '| 载荷 | 历史 SHM MB/s | 当前 SHM MB/s | 变化 | 当前 socket MB/s | SHM/socket | 当前 SHM 三次范围 |','|---|---:|---:|---:|---:|---:|---:|']
raw=[]
for rep in range(1,4):
 vals={}
 for line in (root/f'raw/shm_cli_{rep}.log').read_text().splitlines():
  t=line.split()
  if len(t)==6 and t[0].isdigit(): vals[int(t[0])]=float(t[4])
 raw.append(vals)
def size(label):
 m=re.match(r'([0-9.]+)(B|KB|MB)',label);return int(float(m[1])*{'B':1,'KB':1024,'MB':1048576}[m[2]])
for k,v in new.items():
 if k not in old:continue
 o=old[k][2]; n=v[2]; vals=[r[size(k)] for r in raw]
 text.append(f'| {k} | {o:.1f} | {n:.1f} | {(n/o-1)*100:+.1f}% | {v[3]:.1f} | {n/v[3]:.1f}x | {min(vals):.1f}–{max(vals):.1f} |')
text+=['','## 上层回声接口','',
'当前 bench_app 使用 reader_wait 按需通知，旧版的 immediate/deferred 已移除。下表列出两种历史模式，不能把百分比直接归因为本轮三个修复。当前单进程允许 CPU 2/4，线程可在两核之间调度；历史上层记录未注明相同绑核策略。每档当前运行 3 次，历史每模式只有一个记录。',
'', '| 类型 | batch | 历史 immediate msg/s | 历史 deferred msg/s | 当前中位数 msg/s | 当前范围 | 当前 MB/s |','|---|---:|---:|---:|---:|---:|---:|']
pattern=r'type=(\w+)\s+batch=\s*(\d+).*?rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
h={}
for l in (hist/'bench_app.txt').read_text().splitlines():
 m=re.search(pattern,l)
 if m:h[(m[1],int(m[2]),'deferred' if 'mode=deferred' in l else 'immediate')]=int(m[3])
for kind,batches in [('small',[1,8,64,256]),('big4k',[8,64,256]),('huge1m',[1,2,4])]:
 for b in batches:
  vals=[];mb=[]
  for rep in range(1,4):
   m=re.search(pattern,(root/f'app_{kind}_b{b}_run{rep}.txt').read_text());assert m
   vals.append(int(m[3]));mb.append(float(m[4]))
  text.append(f'| {kind} | {b} | {h[kind,b,"immediate"]:,} | {h[kind,b,"deferred"]:,} | {statistics.median(vals):,.0f} | {min(vals):,}–{max(vals):,} | {statistics.median(mb):.1f} |')
text+=['','## 回声往返延迟','',
'bench_shm 与 bench_socket 各运行三次，进程 CPU affinity 为 2/4，子进程继承。与 11 月 10 日 final 记录比较时，历史 echo 的绑核和构建配置缺少独立环境记录，所以这部分仅作参考。吞吐脚本的 lat(ns) 是总时间/消息数，不是逐消息 RTT；这里使用 echo 基准的真实串行往返平均值。',
'', '| 字节 | 历史 SHM RTT ns | 当前 SHM RTT 中位数 ns | 当前范围 ns | 当前 socket RTT ns |','|---|---:|---:|---:|---:|']
def echo(p):
 d={}
 for l in p.read_text().splitlines():
  t=l.split()
  if len(t)==5 and t[0].isdigit():d[int(t[0])]=float(t[3])
 return d
he=echo(root.parent/'2026-11-10_final/bench_shm.txt')
se=[echo(root/f'bench_shm_run{i}.txt') for i in range(1,4)]
ke=[echo(root/f'bench_socket_run{i}.txt') for i in range(1,4)]
for k in se[0]:
 v=[e[k] for e in se];kv=[e[k] for e in ke]
 text.append(f'| {k} | {he[k]:.0f} | {statistics.median(v):.0f} | {min(v):.0f}–{max(v):.0f} | {statistics.median(kv):.0f} |')
text+=['','环境、编译日志、逐次输出和复跑脚本随本目录归档。历史与当前环境状态不同，并且跨越了此前全部审核修复（通知机制、读取预算等），这些数据用于版本趋势比较，不能隔离最后三处修复的独立开销。codec/batch 微基准输出另行保留，内部使用 best-of-3，不与本报告的三次中位数混合。']
(root/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
