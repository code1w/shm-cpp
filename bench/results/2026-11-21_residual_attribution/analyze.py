#!/usr/bin/env python3
import pathlib,csv,statistics
out=pathlib.Path(__file__).resolve().parent
rs=list(csv.DictReader((out/'samples.csv').open()));pr=list(csv.DictReader((out/'window_profiles.csv').open()))
assert len(rs)==48 and len(pr)==36
variants=['baseline','current','old_loop','old_ring','old_read','direct_dispatch','hs_watch_off','old_loop_hs_off']
def median(data,field):return statistics.median(float(r[field]) for r in data)
def rows(variant,batch):return [r for r in rs if r['variant']==variant and r['batch']==str(batch)]
text=['# 热路径简化后的剩余差距：稳态与短窗口归因','',
'基线ca766b1；当前是已删除预算、按单线程非重入codec契约简化后的工作区。此前针对性1,000,000条、无预热的测试显示仍低13.5%/18.4%；此次检查该差距是否稳定，以及是否来自额外poll fd、环校验、读取逻辑或分发包装。没有修改生产代码。',
'',
'控制：相同-O2 -DNDEBUG、相同benchmark、server线程CPU2/client线程CPU4。先处理1,048,576条，再计时8,388,608条；各variant各档3次旋转反转，共48组。两侧增加相同的用户态poll调用计数，计数使用TLS，测量开关独占cache line，避免额外原子争用。吞吐比较是此相同仪器条件的版本对照。',
'', '## 结论', '',
'持续吞吐剩余差距约1%，没有观察到稳定13%–18%的额外成本。对照预热后的长窗口，batch64基线4,743,340/当前4,690,897 msg/s（各侧中位数比-1.1%），batch256基线12,152,556/当前12,097,364（-0.5%）。',
'',
'替换旧EventLoop/旧RingBuf/旧读取函数、去掉模板分发包装，或诊断性移除握手通知监听，都未给出稳定的大幅收益；不能为追回旧短测数字而删除边界校验、Stop唤醒或握手完成通知。',
'',
'原始二进制的硬件计数说明，当前正常热路径约多4%指令；预热长测每回复CPU周期只多约1%–2%，远小于此前逐包保活时的巨大增加。短测工作状态不一致：例如batch256短测有效用户态周期率基线约2.31/当前2.15 GHz，长测约3.21/3.23 GHz。该值由用户态cycle总数/用户CPU时间推导，是工作区间平均值，不是CPU频率寄存器采样；本实验不能把差距100%分解为governor、uncore、缓存或调度因素。',
'',
'另见notify_startup目录：验证了每包通知模式本身很慢，但本轮慢样本在结束时都已进入按需通知，且通知写入远少于消息数，故不能把残余13%–18%全部归因于通知初始化。',
'', '## 长窗口吞吐与poll次数（三次中位数）','',
'old_loop仅替换旧事件循环；old_ring仅替换旧环头文件；old_read只替换旧DrainRingOnce；direct_dispatch绕过CallCodec/CallCallback模板包装；hs_watch_off单客户端测试中注销握手通知fd；old_loop_hs_off合并两项。回退变体只用于诊断，不具备当前完整正确性保证。',
'', '| 变体 | batch64 msg/s | 相对当前 | batch256 msg/s | 相对当前 |','|---|---:|---:|---:|---:|']
for v in variants:
 cells=[v]
 for b in [64,256]:
  m=median(rows(v,b),'msg_s');base=median(rows('current',b),'msg_s');cells += [f'{m:,.0f}',f'{(m/base-1)*100:+.1f}%']
 text.append('| '+' | '.join(cells)+' |')
text+=['','| 变体 | batch64 server/client polls | batch256 server/client polls |','|---|---:|---:|']
for v in variants:
 text.append('| '+v+' | '+' | '.join(f'{median(rows(v,b),"server_polls"):,.0f} / {median(rows(v,b),"client_polls"):,.0f}' for b in [64,256])+' |')
text+=['','| 变体 | batch64 msg/s范围 | batch256 msg/s范围 |','|---|---:|---:|']
for v in variants:
 cells=[v]
 for b in [64,256]:
  vals=[int(r['msg_s']) for r in rows(v,b)];cells.append(f'{min(vals):,}–{max(vals):,}')
 text.append('| '+' | '.join(cells)+' |')
text+=['','## 原始短测、外部CPU预热、内部预热长测','',
'另外36次perf stat，不与上表合并。short使用未改动的原始1,000,000条binary；primed使用同一binary，但在启动前对CPU2/4各预热400ms并停止预热进程；warm使用上面的1,048,576+8,388,608版本。外部预热没有保证运行区间频率完全一致，不能当成锁定频率实验。计数覆盖整个进程，warm每回复cycle/instructions除以总9,437,184条，包含预热；吞吐本身仅计入后8,388,608条。',
'', '| batch | 窗口 | 版本 | msg/s中位数 | user cycles/reply | user instructions/reply | 有效user GHz |','|---|---|---|---:|---:|---:|---:|']
for b in ['64','256']:
 for window in ['short','primed','warm']:
  for side in ['baseline','current']:
   data=[r for r in pr if r['batch']==b and r['window']==window and r['side']==side];assert len(data)==3
   text.append(f'| {b} | {window} | {side} | {median(data,"msg_s"):,.0f} | {median(data,"user_cycles_per_total_reply"):.1f} | {median(data,"user_instructions_per_total_reply"):.1f} | {median(data,"effective_user_GHz"):.2f} |')
text+=['','## 边界与建议','',
'此前13.5%/18.4%描述了该组未预热短突发样本，不能据此认定持续吞吐还有同样幅度退化。若业务关注短突发，应独立测冷启动、唤醒和负载/频率变化下的延迟；稳态吞吐应预热、延长计时窗口并保留原始范围。此次没有单条消息p99、锁频实验，也没有精确测uncore频率。',
'',
'当前保留的返回长度、环游标、关闭状态和异常处理检查有约4%指令差异，但长测周期和吞吐差异很小；没有证据支持继续删除它们。源码、补丁、构建、测量CSV、poll探针及perf原始文本都随目录归档。']
(out/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
