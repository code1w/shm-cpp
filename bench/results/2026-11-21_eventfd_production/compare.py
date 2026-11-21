#!/usr/bin/env python3
import pathlib,re,statistics
root=pathlib.Path(__file__).resolve().parent
previous=root.parent/'2026-11-21_after_audit'
oldest=root.parent/'2026-11-10_o2_after_p15'
def throughput(p):
 result={}
 for line in p.read_text().splitlines():
  t=line.split()
  if len(t)==6 and re.fullmatch(r'[0-9.]+(?:B|KB|MB)',t[0]):result[t[0]]=[float(x) for x in t[1:5]]
 return result
old=throughput(previous/'bench_sh.txt');new=throughput(root/'bench_sh.txt')
text=['# 生产 eventfd 性能复测（2026-11-21）','',
'使用切回 eventfd 后的工作区 build/ 生产产物，Release `-O2 -DNDEBUG`，无 sanitizer。与切回前的 2026-11-21_after_audit 比较；两轮吞吐均为 server CPU2 / client CPU4，每档三次中位数，echo 和 bench_app 均使用进程 affinity CPU2,4、线程可在两核之间调度。该条件与单独交错 A/B 中 server/client 线程分别固定 CPU2/4 不同，不混合解读两类结果。',
'',
'全部 44 次运行成功：6 次端到端吞吐、6 次串行 echo、30 次上层回声、2 项内部 best-of-3 微基准。MB/s 沿用程序标签，实际为 MiB/s。主表是两次独立整轮复测，不是交错 A/B；时间、调度与系统负载变化也会影响差值。',
'', '## 单向吞吐', '', '| 载荷 | 切回前 socketpair MiB/s | 当前 eventfd MiB/s | 变化 | 当前 socket MiB/s | SHM/socket |','|---|---:|---:|---:|---:|---:|']
for k,v in new.items():
 o=old[k][2];n=v[2]
 text.append(f'| {k} | {o:.1f} | {n:.1f} | {(n/o-1)*100:+.1f}% | {v[3]:.1f} | {n/v[3]:.1f}x |')
pattern=r'type=(\w+)\s+batch=\s*(\d+).*?rate=\s*(\d+) msg/s\s+([\d.]+) MB/s'
def app(folder,kind,batch):
 vals=[];mb=[]
 for rep in range(1,4):
  m=re.search(pattern,(folder/f'app_{kind}_b{batch}_run{rep}.txt').read_text());assert m
  vals.append(int(m[3]));mb.append(float(m[4]))
 return statistics.median(vals),statistics.median(mb),min(vals),max(vals)
h={}
for l in (oldest/'bench_app.txt').read_text().splitlines():
 m=re.search(pattern,l)
 if m and 'mode=deferred' in l:h[m[1],int(m[2])]=int(m[3])
text+=['','## 上层接口回声吞吐','',
'旧历史 deferred 模式已移除，最后一列只列历史数值作背景，不是相同模式的即时对照。当前和切回前均有相同读取预算、reader_wait 与安全修复。',
'', '| 类型 | batch | 切回前 msg/s | 当前 msg/s | 变化 | 当前范围 msg/s | 当前 MiB/s | 11月10日 deferred msg/s |','|---|---:|---:|---:|---:|---:|---:|---:|']
for kind,batches in [('small',[1,8,64,256]),('big4k',[8,64,256]),('huge1m',[1,2,4])]:
 for batch in batches:
  o=app(previous,kind,batch);n=app(root,kind,batch)
  text.append(f'| {kind} | {batch} | {o[0]:,.0f} | {n[0]:,.0f} | {(n[0]/o[0]-1)*100:+.1f}% | {n[2]:,}–{n[3]:,} | {n[1]:.1f} | {h[kind,batch]:,} |')
def echo(p):
 result={}
 for l in p.read_text().splitlines():
  t=l.split()
  if len(t)==5 and t[0].isdigit():result[int(t[0])]=float(t[3])
 return result
old_echo=[echo(previous/f'bench_shm_run{i}.txt') for i in range(1,4)]
new_echo=[echo(root/f'bench_shm_run{i}.txt') for i in range(1,4)]
new_socket=[echo(root/f'bench_socket_run{i}.txt') for i in range(1,4)]
text+=['','## 串行回声往返延迟','',
'这里使用 bench_shm 的平均 RTT，再取三次中位数；不是吞吐脚本中总时间/条数的摊销指标。',
'', '| 字节 | 切回前 SHM RTT ns | 当前 SHM RTT ns | 变化 | 当前 SHM范围 ns | 当前 socket RTT ns |','|---|---:|---:|---:|---:|---:|']
for k in new_echo[0]:
 o=statistics.median(e[k] for e in old_echo);nv=[e[k] for e in new_echo];n=statistics.median(nv)
 text.append(f'| {k} | {o:.0f} | {n:.0f} | {(n/o-1)*100:+.1f}% | {min(nv):.0f}–{max(nv):.0f} | {statistics.median(e[k] for e in new_socket):.0f} |')
text+=['','## 数据与复跑','',
'环境在 env.txt，生产头文件身份在 source.sha256，构建在 build.log。原始端到端日志在 raw/；echo、上层接口和微基准逐次输出在本目录。先构建工作区 Release -O2，再依次运行 run_throughput.sh、run_extra.py、compare.py。脚本会覆盖该目录的结果，复跑请使用新结果目录。',
'',
'生产 eventfd 不再需要 A/B 实验中的额外 dup，本地 WakeReader 直接写 NotifyReadFd；通知 syscall/合并语义与 eventfd 实验一致。共享 fd 的 O_NONBLOCK 属性必须由双方保持。']
summary=[]
for batch in [1,8,64,256]:
 o=app(previous,'small',batch)[0];n=app(root,'small',batch)[0]
 summary.append(f'batch{batch} {n:,.0f} msg/s（{(n/o-1)*100:+.1f}%）')
current=app(root,'small',256)[0]
text[4:4]=['## 主要观察', '', '上层小消息吞吐提高：'+ '，'.join(summary)+'。端到端 raw 单向吞吐变化在 -2.7% 到 +1.2%，整体接近上一轮。', '', f'小消息 batch256 当前仍比 11 月 10 日旧 deferred 的 6,039,311 msg/s 低 {(1-current/6039311)*100:.1f}%。本次生产整轮复测的 batch256 提升幅度大于交错 A/B 的典型差异，说明整轮时间和线程调度的波动也不可忽略；隔离通知后端的因果结论应优先参考 notify_ab 报告。', '']
(root/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
