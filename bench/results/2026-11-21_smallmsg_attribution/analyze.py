#!/usr/bin/env python3
import pathlib,csv,statistics,hashlib
out=pathlib.Path(__file__).resolve().parent
single=list(csv.DictReader((out/'samples.csv').open()));layers=list(csv.DictReader((out/'samples_layers.csv').open()));cpu=list(csv.DictReader((out/'cpu_per_request.csv').open()))
assert len(single)==80 and len(layers)==48 and len(cpu)==18
variants=['before','current','send_self_off','send_codec_off','decode_codec_scope','callback_snapshot_off','buffer_move_off','combined']
layer_variants=['before','current','legacy_sender','legacy_reader','legacy_callback_storage','legacy_sender_reader','legacy_all_hot','legacy_connection']
def values(rows,v,batch):return [int(r['msg_s']) for r in rows if r['variant']==v and r['batch']==str(batch)]
def median(rows,v,batch):return statistics.median(values(rows,v,batch))
text=['# 无读取预算后小消息吞吐退化归因（2026-11-21）','',
'基线为ca766b1；当前为已删除读取预算的工作区快照。相同bench_app，Release -O2 -DNDEBUG，server线程CPU2/client线程CPU4。第一阶段8变体×2档×5次，共80次；第二阶段8变体×2档×3次，共48次；顺序轮换并反转。另有18次用户态硬件计数与1次采样，不计入吞吐中位数。',
'',
'生产代码没有改动。取消保活和恢复旧热路径的副本仅用于不重入的本基准诊断，会失去已验证的重入安全；不能直接用于生产。结果按阶段分别比较，不将不同阶段中位数混用，也不把单因素收益相加。',
'', '## 主要结论', '',
'剩余损失主要来自ShmConnection逐包生命周期保护及回调/发送/读取热路径结构，而非已删除的读取预算。新增连接/codec/回调强引用保活会进行原子引用计数；weak_from_this().lock()还创建临时weak_ptr，增加弱引用计数更新。发送缓冲vector在每次调用前转移所有权、返回时归还，加上回调的额外指针访问、返回后身份/状态检查，增加了指令量和依赖访存。vector移动不复制128KiB数据，不能把它误解释为大块payload复制。',
'',
'硬件计数：batch256当前每个完整请求/回声约1397条用户态指令、1175个用户态周期；基线约950条、452个周期。指令增加约47%，周期增加约160%。batch64也呈相同趋势。数据包含client/server所有线程与初始化摊销，不是SendCmd单函数的cycle精确成本。',
'',
'采样共581个cycles:u样本，未丢样本：约23%在DrainRingOnce，20%在SendCmd，16%在shared_ptr引用释放_M_release；Encode和Decode各约4%，memmove约1.5%。这些比例包含旧版原本就有的操作，不是“新增成本占比”。发送反汇编可见lock cmpxchg、lock add和lock xadd引用计数操作。',
'',
'逐层恢复旧版发送/读取函数和内嵌std::function回调存储后，同阶段batch64从3008273提升到4199869 msg/s（+39.6%）；batch256从3793268提升到6917458（+82.4%），与同阶段基线仅差约7.3%和6.3%。因此已定位主要损失所在。该阶段只有3次，且范围较宽，剩余差距没有单独归因到外围EventLoop、对象布局或代码生成因素，不能宣称已做100%精确成本分解。',
'', '## 单因素定义', '',
'- send_self_off：只取消SendCmd内的连接保活。',
'- send_codec_off：只取消SendCmd内codec强引用，使用原始指针并保留身份比较。',
'- decode_codec_scope：Decode的codec强引用移到连续批次外；每帧检查并按需更新成员变化，保留回调返回后的身份/状态校验。',
'- callback_snapshot_off：回调调用期间不复制StableCallback的shared_ptr快照。',
'- buffer_move_off：借用成员vector而不移动所有权，保留发送重入标志；此变体不防止编码重入ResizeBuffer使借用指针失效。',
'- combined：同时应用上面五项，边界校验/状态检查及其它模块继续使用当前版本。',
'', '## 第一阶段吞吐（五次中位数）', '',
'百分比为各变体中位数/current中位数−1。范围和原始样本随表/CSV保留，变化不是独立可加成本。',
'', '| 变体 | batch64 msg/s | 相对current | batch256 msg/s | 相对current |','|---|---:|---:|---:|---:|']
for v in variants:
 cells=[v]
 for b in [64,256]:
  m=median(single,v,b);base=median(single,'current',b);cells += [f'{m:,.0f}',f'{(m/base-1)*100:+.1f}%']
 text.append('| '+' | '.join(cells)+' |')
text+=['','| 变体 | batch64范围 | batch256范围 |','|---|---:|---:|']
for v in variants:
 text.append('| '+v+' | '+' | '.join(f'{min(values(single,v,b)):,}–{max(values(single,v,b)):,}' for b in [64,256])+' |')
text+=['','## 第二阶段：逐层替换旧实现（三次中位数）', '',
'legacy_sender只替换SendCmd和其旧缓冲扩容函数；legacy_reader只替换DrainRing/DrainRingOnce；legacy_callback_storage仅把ShmConnection的四个StableCallback成员恢复为内嵌std::function。legacy_sender_reader合并发送/读取；legacy_all_hot合并三项；legacy_connection替换整个ShmConnection头文件。其它模块都保持当前源码。这些副本不具备当前完整的重入安全，不是可交付修复。',
'', '| 变体 | batch64 msg/s | 相对current | batch256 msg/s | 相对current |','|---|---:|---:|---:|---:|']
for v in layer_variants:
 cells=[v]
 for b in [64,256]:
  m=median(layers,v,b);base=median(layers,'current',b);cells += [f'{m:,.0f}',f'{(m/base-1)*100:+.1f}%']
 text.append('| '+' | '.join(cells)+' |')
text+=['','| 变体 | batch64范围 | batch256范围 |','|---|---:|---:|']
for v in layer_variants:
 text.append('| '+v+' | '+' | '.join(f'{min(values(layers,v,b)):,}–{max(values(layers,v,b)):,}' for b in [64,256])+' |')
text+=['','## 硬件计数（每完整请求/回声，三次中位数）', '',
'perf stat测量cycles:u与instructions:u，包含两端用户态工作，不含内核成本；这18次单独运行，不计入吞吐结果。组合组不是legacy_all_hot，是第一阶段的combined。',
'', '| batch | 变体 | 用户态cycles/request | 用户态instructions/request |','|---|---|---:|---:|']
for b in ['64','256']:
 for v in ['before','current','combined']:
  rs=[r for r in cpu if r['batch']==b and r['variant']==v];assert len(rs)==3
  text.append(f'| {b} | {v} | {statistics.median(float(r["user_cycles_per_request"]) for r in rs):.1f} | {statistics.median(float(r["user_instructions_per_request"]) for r in rs):.1f} |')
text+=['','## 后续优化方向', '',
'优先按批保活codec和消息回调，并在每帧保持变化检测；减少内部已有强引用保护时重复的连接保活；编码借用期间延后缓冲扩容，减少vector所有权往返；检查热函数的额外指针访问、分支与栈保存。SendCmd机器码由基线1146字节增至当前2120字节，说明保护同时改变了代码生成，但本实验没有把函数大小当作独立性能原因。',
'',
'所有候选都必须重新通过关闭当前连接、替换codec/回调、ResizeBuffer重入、异常退出和读写边界回归；不要简单删除已证实必要的安全检查。预算已删除，因此继续调整64帧/256KiB阈值不是本轮的修复方向。',
'',
'文件：samples.csv/samples_layers.csv为吞吐原始记录，cpu_per_request.csv为计数摘要；每变体补丁、构建日志和源码指纹随目录保存。current.perf.data为采样记录，perf_report_current.txt和perf_annotate_sendcmd.txt为导出文本。采样符号报告出现系统库DWARF范围警告，函数级自开销与反汇编可读，但不据此声称全部内联调用栈已准确展开。']
(out/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
