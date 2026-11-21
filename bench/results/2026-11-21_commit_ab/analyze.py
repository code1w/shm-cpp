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

text=['# 本次修改前后 SHM 性能交错对比（2026-11-21）','',
'修改前：`ca766b1`；修改后：`76d4c83`。比较整个提交的变更，两侧最终均使用 eventfd，没有使用 socketpair 版本或旧历史日志替代修改前基线。源代码由 git archive 固定提交提取，临时目录位于 /tmp/shm-commit-ab。',
'',
'用户确认延迟维度为 RTT。两侧相同 GCC、Release `-O2 -DNDEBUG`、无 sanitizer、相同 protobuf 3.21.12 依赖。server/client/bench_shm/bench_app 原始基准文件在两个提交中逐字节相同；echo 进程和 app 线程两侧均增加相同的绑核补丁，服务端 CPU2、客户端 CPU4（不同物理核、同 NUMA）。不修改生产代码。',
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
'使用同一 bench_app（ShmServer/ShmAsyncClient/SimplePodCodec），同消息数、同 batch。这里保留整个提交对读取预算、回调和编码生命周期等的影响，与 raw 数据分层。batch1 每次请求等完整回声后再发送下一条，其摊销平均回声 RTT 为 1e9 / msg_s；批量吞吐的倒数不作为 RTT。',
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
'五对交错仍不能排除系统负载与 CPU 频率的全部影响，特别是大包和批量上层消息的波动。没有修改读取预算以匹配旧实现，因为该预算本身属于本次修改；其引起的吞吐变化应计入整个提交的影响。',
'',
'环境及提交全哈希在 env.txt；两側源文件 SHA256、相同绑核补丁与原始输出随目录归档。初次最小配置没有启用 protobuf，两个提交的 demo 在非 protobuf 分支均编译失败；已改用与生产相同的 protobuf 配置，两侧成功构建后才开始计时。该配置失败日志保留供审计，未修改 demo 源码。',
'',
'复跑：运行 prepare.py 准备两侧临时源码，再分别 cmake 配置 Release -O2 并构建；运行 run.py，最后 analyze.py。脚本会覆盖本目录采样文件，复跑须新建结果目录。该实验仅使用固定的已提交版本，未改动或推送代码。']
observations=['## 主要结论', '',
'Raw SHM 单向吞吐与忙轮询 RTT 整体接近修改前：吞吐配对变化中位数各档为 -3.3% 到 +3.9%，RTT 为 -1.9% 到 +4.0%。小载荷吞吐个别配对波动可达40%以上，不能把单次结果当作稳定变化。64KiB 到512KiB等大载荷吞吐多数变化在1%以内；64KiB RTT 五对均降低，中位数约 -1.9%。', '',
'上层接口出现明确吞吐退化，不能据 raw 结果认定整个提交没有性能损失：small/batch64 配对中位数 -19.6%，small/batch256 -58.8%（五对均下降，范围 -61.7% 到 -41.4%）；big4k/batch256 -15.7%；huge1m/batch4 -17.8%。这次对比包括整个提交的读取预算与编解码生命周期修复，没有进一步拆分各项改动的成本。', '',
'上层 small/batch1 吞吐从五次中位数 99,070 降到 95,862 msg/s，对应串行平均回声 RTT 10.094µs → 10.432µs。该延迟包含应用 codec 与事件驱动处理，不能和约400ns的 raw 忙轮询 RTT 混为同一指标。', '']
text[8:8]=observations
(out/'README.md').write_text('\n'.join(text)+'\n')
with (out/'summary.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['dimension','payload_or_kind','batch','before_median','after_median','paired_delta_pct','min_delta_pct','max_delta_pct','before_min','before_max','after_min','after_max']);w.writerows(summary)
print('\n'.join(text))
