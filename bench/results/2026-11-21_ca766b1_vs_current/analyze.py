#!/usr/bin/env python3
import csv,pathlib,statistics
out=pathlib.Path(__file__).resolve().parent
rows={name:list(csv.DictReader((out/f'{name}.csv').open())) for name in ['throughput','rtt','app']}
assert len(rows['throughput'])==190
assert len(rows['rtt'])==110
assert len(rows['app'])==100

def stats(data,key,field):
 b=[r for r in data if all(r[k]==v for k,v in key.items()) and r['side']=='before']
 a=[r for r in data if all(r[k]==v for k,v in key.items()) and r['side']=='after']
 assert len(b)==len(a)==5,(key,len(b),len(a))
 bv=[float(r[field]) for r in b];av=[float(r[field]) for r in a]
 ds=[]
 for br in b:
  ar=next(r for r in a if r['round']==br['round'])
  ds.append((float(ar[field])/float(br[field])-1)*100)
 return statistics.median(bv),statistics.median(av),statistics.median(ds),min(ds),max(ds),min(bv),max(bv),min(av),max(av)

text=['# ca766b1 与当前无读取预算代码的完整 SHM 对比（2026-11-21）','',
'基线：`ca766b1`；当前：以 `76d4c83` 为基础、已删除读取预算的工作区源码（包含未提交改动），不是76d4c83原提交。基线从git archive提取，当前源码独立复制到临时目录，两侧都使用eventfd。当前相对基线的完整源码补丁在 current_vs_ca766b1.patch，文件SHA256随目录保存；临时源码位于 /tmp/shm-ca766b1-vs-current。',
'',
'用户确认延迟维度为 RTT。两侧相同 GCC、Release `-O2 -DNDEBUG`、无 sanitizer、相同 protobuf 3.21.12 依赖。server/client/bench_shm/bench_app 原始基准文件在基线与当前代码中逐字节相同；echo 进程和 app 线程两侧均增加相同的绑核补丁，服务端 CPU2、客户端 CPU4（不同物理核、同 NUMA）。不修改生产代码。',
'',
'每项五对交错 before/after、after/before，全部 120 组基准成功：10 组 raw 吞吐、10 组忙轮询 RTT、100 组上层接口。共 400 个逐档测量记录。吞吐越高越好，RTT 越低越好。两侧各自五次中位数与配对变化中位数分开计算，列间直接相除可能与配对百分比略有区别；五对范围描述波动，不是统计置信区间。',
'', '## 1. Raw SHM 单向吞吐', '',
'使用相同的 server --bench / client --bench，十九个载荷档位；消息条数保持原基准配置，每组 client/server 显式绑核4/2。MiB/s 按 1024² 字节计算。该数据是单向持续发送吞吐，不是 RTT 倒数。',
'', '| 载荷 B | 修改前 MiB/s | 修改后 MiB/s | 配对变化中位数 | 五对变化范围 | 修改前样本范围 | 修改后样本范围 |','|---|---:|---:|---:|---:|---:|---:|']
summary=[]
for payload in dict.fromkeys(r['payload'] for r in rows['throughput']):
 s=stats(rows['throughput'],{'payload':payload},'mib_s')
 text.append(f'| {payload} | {s[0]:.1f} | {s[1]:.1f} | {s[2]:+.1f}% | {s[3]:+.1f}%–{s[4]:+.1f}% | {s[5]:.1f}–{s[6]:.1f} | {s[7]:.1f}–{s[8]:.1f} |')
 summary.append(['throughput',payload,'',*s])
text+=['','## 2. Raw SHM 回声 RTT','',
'使用相同 bench_shm，客户端每次发送一条消息并等回声，服务端忙轮询消费并原样返回。不调用 NotifyPeer，因此测量的是底层共享内存复制/同步的串行往返延迟，不包含事件驱动睡眠唤醒、codec 或握手时间。每档预热1000轮，正式轮数与原程序相同（5000–100000）。表中是每组平均 RTT 的五组中位数，不是单条消息 p50/p99。',
'', '| 载荷 B | 修改前 RTT ns | 修改后 RTT ns | 配对变化中位数 | 五对变化范围 | 修改前样本范围 ns | 修改后样本范围 ns |','|---|---:|---:|---:|---:|---:|---:|']
for payload in dict.fromkeys(r['payload'] for r in rows['rtt']):
 s=stats(rows['rtt'],{'payload':payload},'mean_rtt_ns')
 text.append(f'| {payload} | {s[0]:.0f} | {s[1]:.0f} | {s[2]:+.1f}% | {s[3]:+.1f}%–{s[4]:+.1f}% | {s[5]:.0f}–{s[6]:.0f} | {s[7]:.0f}–{s[8]:.0f} |')
 summary.append(['rtt',payload,'',*s])
text+=['','## 3. 上层 SHM 接口吞吐与串行回声时间','',
'使用同一 bench_app（ShmServer/ShmAsyncClient/SimplePodCodec），同消息数、同 batch。两侧都不设置读取预算，当前仍保留回调和编码生命周期等安全修复，与 raw 数据分层。batch1 每次请求等完整回声后再发送下一条，其摊销平均回声 RTT 为 1e9 / msg_s；批量吞吐的倒数不作为 RTT。',
'', '| 消息 | batch | 修改前 msg/s | 修改后 msg/s | 配对变化中位数 | 五对变化范围 | 修改前范围 | 修改后范围 |','|---|---:|---:|---:|---:|---:|---:|---:|']
for kind,batch in dict.fromkeys((r['kind'],r['batch']) for r in rows['app']):
 s=stats(rows['app'],{'kind':kind,'batch':batch},'msg_s')
 text.append(f'| {kind} | {batch} | {s[0]:,.0f} | {s[1]:,.0f} | {s[2]:+.1f}% | {s[3]:+.1f}%–{s[4]:+.1f}% | {s[5]:,.0f}–{s[6]:,.0f} | {s[7]:,.0f}–{s[8]:,.0f} |')
 summary.append(['app',kind,batch,*s])
text+=['', '| 串行请求类型（batch1） | 修改前平均回声 RTT µs | 修改后平均回声 RTT µs |','|---|---:|---:|']
for kind in ['small','huge1m']:
 s=stats(rows['app'],{'kind':kind,'batch':'1'},'msg_s')
 text.append(f'| {kind} | {1e6/s[0]:.3f} | {1e6/s[1]:.3f} |')
text+=['','## 边界、环境与复跑','',
'五对交错仍不能排除系统负载与 CPU 频率的全部影响，特别是大包和批量上层消息的波动。当前版本已按用户选择删除读取预算，时间/帧数/字节数上限和预算自唤醒都不存在；此次衡量当前代码整体与基线的差异。',
'',
'环境及基线/当前HEAD全哈希在env.txt；两侧源文件SHA256、工作区相对基线的源码补丁、相同绑核补丁与原始输出随目录归档。两侧使用相同protobuf配置，成功构建后才开始计时。',
'',
'复跑：运行 prepare.py 准备两侧临时源码，再分别 cmake 配置 Release -O2 并构建；运行 run.py，最后 analyze.py。脚本会覆盖本目录采样文件，复跑须新建结果目录。当前测量的是执行准备脚本时的工作区快照；复跑若工作区已变更，须按保存补丁重建同一快照。实验未改动生产代码或推送提交。']
observations=['## 主要结论', '']
for payload in ['64','4096','65536','1048576']:
 s=stats(rows['throughput'],{'payload':payload},'mib_s')
 observations.append(f'- Raw {payload}B 单向吞吐：{s[0]:.1f} → {s[1]:.1f} MiB/s，五对变化中位数 {s[2]:+.1f}%（范围 {s[3]:+.1f}%–{s[4]:+.1f}%）。')
for kind,batch in [('small','64'),('small','256'),('big4k','256'),('huge1m','4')]:
 s=stats(rows['app'],{'kind':kind,'batch':batch},'msg_s')
 observations.append(f'- 上层 {kind}/batch{batch}：{s[0]:,.0f} → {s[1]:,.0f} msg/s，五对变化中位数 {s[2]:+.1f}%（范围 {s[3]:+.1f}%–{s[4]:+.1f}%）。')
s=stats(rows['app'],{'kind':'small','batch':'1'},'msg_s')
observations.extend(['',f'上层small/batch1的串行平均回声时间：{1e6/s[0]:.3f} → {1e6/s[1]:.3f}µs。该指标包含codec与事件驱动处理，不等同于下面的raw忙轮询RTT。', ''])
text[8:8]=observations
(out/'README.md').write_text('\n'.join(text)+'\n')
with (out/'summary.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['dimension','payload_or_kind','batch','before_median','after_median','paired_delta_pct','min_delta_pct','max_delta_pct','before_min','before_max','after_min','after_max']);w.writerows(summary)
print('\n'.join(text))
