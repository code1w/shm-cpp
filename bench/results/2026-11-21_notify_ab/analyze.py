#!/usr/bin/env python3
import csv, statistics, pathlib, hashlib
out=pathlib.Path(__file__).resolve().parent
rows=list(csv.DictReader((out/'samples.csv').open()))
assert len(rows)==100,len(rows)
keys=list(dict.fromkeys((r['kind'],int(r['batch'])) for r in rows))
text=['# eventfd / socketpair 交错 A/B（2026-11-21）','',
'目的：隔离通知后端对当前代码性能的影响，不使用旧版代码作为基线。实验使用当前工作区的相同快照，仅替换 RingChannel 通知的创建、fd 标签/验证、发送与排空。eventfd 用非阻塞 eventfd 的 write/read；socketpair 保持当前 Unix datagram send/recv。reader_wait、读取预算（64 帧 / 256 KiB / 2ms）、codec、安全修复、内存布局、通知触发时机全部相同。内部握手仍使用 eventfd；eventfd 版本的 DrainNotify 直接 read，socket 版本保留现有 recv 后 ENOTSOCK 回退，握手不在正常数据面计时内。',
'',
'两侧 Release `-O2 -DNDEBUG`、同编译器、不启用 sanitizer。基准代码两侧均增加相同的线程绑核：server CPU2、client CPU4（不同物理核、同 NUMA）。每档 5 对、共 100 次运行，轮次和档位间交替 E/S 与 S/E；任何运行失败都会停止。每边 3 个兼容性/回归测试均通过。所有其它源文件逐字节相同，通知补丁、相同绑核补丁、源文件 SHA256、环境、编译日志和逐次输出随目录保存。',
'',
'## 吞吐结果','',
'变化定义：同一对内 socketpair / eventfd − 1，取五对中位数；负数表示 socketpair 更慢。吞吐列分别取两侧五次中位数，因此吞吐列相除与配对变化可能略有不同。MB/s 沿用程序标签，实际为 MiB/s；这是应用层请求/回声吞吐，不是裸通知 syscall 延迟。',
'', '| 消息 | batch | eventfd msg/s | socketpair msg/s | 配对变化中位数 | 五对变化范围 | socket 较慢次数 | eventfd MiB/s | socket MiB/s |',
'|---|---:|---:|---:|---:|---:|---:|---:|---:|']
results=[]
for kind,batch in keys:
 e=[r for r in rows if r['kind']==kind and int(r['batch'])==batch and r['backend']=='eventfd']
 s=[r for r in rows if r['kind']==kind and int(r['batch'])==batch and r['backend']=='socketpair']
 assert len(e)==len(s)==5
 em=statistics.median(float(r['msg_s']) for r in e);sm=statistics.median(float(r['msg_s']) for r in s)
 deltas=[]
 for er in e:
  sr=next(r for r in s if r['round']==er['round']);deltas.append((float(sr['msg_s'])/float(er['msg_s'])-1)*100)
 delta=statistics.median(deltas)
 text.append(f'| {kind} | {batch} | {em:,.0f} | {sm:,.0f} | {delta:+.1f}% | {min(deltas):+.1f}%–{max(deltas):+.1f}% | {sum(d<0 for d in deltas)}/5 | {statistics.median(float(r["mib_s"]) for r in e):.1f} | {statistics.median(float(r["mib_s"]) for r in s):.1f} |')
 results.append({'kind':kind,'batch':batch,'eventfd_msg_s':em,'socketpair_msg_s':sm,'paired_delta_pct':delta,'min_delta_pct':min(deltas),'max_delta_pct':max(deltas)})
text+=['','## 结论', '', '只切换通知后端时，socketpair 的 small/batch1 与 batch8 配对吞吐分别下降 22.6% 和 21.6%，五对方向一致、变化范围较窄，支持频繁通知场景存在明确性能成本。small/batch256 的配对中位数仅下降 2.9%，范围 -15.5% 到 +3.9%，不能将前一轮跨历史版本的 43.9% 下降主要解释为 socketpair 的成本。big4k/batch64 和 batch256 的配对中位数下降 36.8% 和 32.5%，五对均下降，但下降幅度很不稳定，应保留全部样本而不挑选最快结果。huge1m/batch4 配对变化 +0.2%，范围 -3.9% 到 +4.7%，本轮未观察到稳定下降。', '', '这些差异隔离了当前代码中通知后端整体的影响（创建/传递、发送、排空与计数合并语义），没有进一步隔离发送与接收 syscall 的独立开销。固定读取预算与后端之间的交互也包含在结果内。', '', '## 吞吐样本范围','', '| 消息 | batch | eventfd msg/s 范围 | socketpair msg/s 范围 |','|---|---:|---:|---:|']
for kind,batch in keys:
 v={b:[int(r['msg_s']) for r in rows if r['kind']==kind and int(r['batch'])==batch and r['backend']==b] for b in ['eventfd','socketpair']}
 text.append(f'| {kind} | {batch} | {min(v["eventfd"]):,}–{max(v["eventfd"]):,} | {min(v["socketpair"]):,}–{max(v["socketpair"]):,} |')
text+=['','## 边界与复跑','',
'实验 eventfd 实现仅存在于 /tmp/shm-notify-ab/eventfd，未替换仓库生产通知后端。eventfd 的共享阻塞属性风险仍是切换到 socketpair 的原因；本实验测试的是遵守协议的对端，没有重做不可信对端属性修改测试。性能比较不能证明 eventfd 与 socketpair 的安全能力等价。',
'',
'准备脚本 `prepare.py` 复制执行时的工作区到 /tmp/shm-notify-ab，并归档唯一通知补丁与两边相同的绑核补丁。两侧分别使用 `cmake -S /tmp/shm-notify-ab/<backend> -B /tmp/shm-notify-ab/<backend>/build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG"` 和 `cmake --build ... -j3` 构建；随后运行 `python3 run.py`，结束后 `python3 analyze.py`。复跑会覆盖本目录采样文件，应使用新结果目录保留本次记录。',
'',
'五对测试用于评估本机当前场景，样本范围和方向一致性比单次差值更有意义。本实验没有 syscall 级剖析，无法将成本进一步分解为发送、排空、通知合并或调度；也不能据此直接解释全部历史版本的差异。']
(out/'README.md').write_text('\n'.join(text)+'\n')
with (out/'summary.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(results[0]));w.writeheader();w.writerows(results)
print('\n'.join(text))
