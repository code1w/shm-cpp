#!/usr/bin/env python3
import csv,statistics,pathlib
out=pathlib.Path(__file__).resolve().parent
rows=list(csv.DictReader((out/'samples.csv').open()));assert len(rows)==54
variants=['before','current','no_budget','relaxed_caps','no_frame_clock','codec_scope']
cases=[('small','64'),('small','256'),('huge1m','4')]
text=['# 应用层性能退化诊断（2026-11-21）','',
'固定修改前 ca766b1 / 修改后 76d4c83 的源快照；所有诊断变体基于修改后，仅改动单个诊断因素。相同 bench_app、Release -O2 -DNDEBUG、server CPU2/client CPU4。每变体每档3次，旋转并反转顺序，共54次运行。对比单因素变体时其余安全措施保留；没有改动生产代码。',
'', '## 变体', '',
'- before：修改前代码，作为上下文基线。',
'- current：当前已提交代码。',
'- no_budget：仅禁用读取预算，读到空/不完整帧；用于识别预算整体影响，不作为生产修复建议。',
'- relaxed_caps：仅将64帧/256KiB上限改为1024帧/8MiB，保留2ms时间预算及逐帧时钟检查。',
'- no_frame_clock：仅取消Consume中的逐帧时钟检查，保留64帧/256KiB上限（初始化时钟仍在）。',
'- codec_scope：仅将Decode使用的codec强引用保活移到连续批次外层；逐帧检查成员是否变更，变更时更新引用。其余预算、发送和回调保护不变。',
'', '## 吞吐结果（每档三次中位数，msg/s）','',
'百分比为各变体中位数与同轮current中位数之比，不是单因素成本的可加分解；时序和缓存等交互会使变化非线性。',
'', '| 变体 | small batch64 | 相对current | small batch256 | 相对current | huge1m batch4 | 相对current |',
'|---|---:|---:|---:|---:|---:|---:|']
med={};ranges={}
for v in variants:
 for k,b in cases:
  rs=[int(r['msg_s']) for r in rows if r['variant']==v and r['kind']==k and r['batch']==b];assert len(rs)==3
  med[v,k,b]=statistics.median(rs);ranges[v,k,b]=(min(rs),max(rs))
for v in variants:
 cells=[v]
 for k,b in cases:
  m=med[v,k,b];base=med['current',k,b]
  cells.extend([f'{m:,.0f}',f'{(m/base-1)*100:+.1f}%'])
 text.append('| '+' | '.join(cells)+' |')
text+=['','## 样本范围','', '| 变体 | small batch64 msg/s | small batch256 msg/s | huge1m batch4 msg/s |','|---|---:|---:|---:|']
for v in variants:
 text.append('| '+v+' | '+' | '.join(f'{ranges[v,k,b][0]:,}–{ranges[v,k,b][1]:,}' for k,b in cases)+' |')
phase2=list(csv.DictReader((out/'samples_phase2.csv').open()));assert len(phase2)==45
text+=['','## 第二组：发送与回调保活成本', '',
'第二组仍使用相同基准和配置，每变体每档3次，轮换顺序，共45次；有独立的before/current对照，因此不跨组混用百分比。这里移除部分保护只用于不重入的本基准测成本，不能作为生产补丁：no_callback_snapshot取消StableCallback的回调强引用快照；no_send_self取消SendCmd的连接强引用保活；no_send_buffer_move把独立借用缓冲改为直接引用成员vector。这些变体在允许回调重入、删除owner或扩容缓冲时可能重新暴露已复现的释放后使用。生产代码保持完整保护。', '',
'| 变体 | small batch64 msg/s | 相对current | small batch256 msg/s | 相对current | huge1m batch4 msg/s | 相对current |','|---|---:|---:|---:|---:|---:|---:|']
vs=['before','current','no_callback_snapshot','no_send_self','no_send_buffer_move']
for v in vs:
 cells=[v]
 for k,b in cases:
  vals=[int(r['msg_s']) for r in phase2 if r['variant']==v and r['kind']==k and r['batch']==b];assert len(vals)==3
  base=statistics.median(int(r['msg_s']) for r in phase2 if r['variant']=='current' and r['kind']==k and r['batch']==b)
  m=statistics.median(vals);cells.extend([f'{m:,.0f}',f'{(m/base-1)*100:+.1f}%'])
 text.append('| '+' | '.join(cells)+' |')
text+=['','## 诊断结论', '',
'1MB/batch4的退化主要与256KiB读取上限有关：仅放宽帧/字节上限、保留时间预算时，从2157提升到2545 msg/s（+18.0%），接近同组旧版2614；取消时钟或减少保活仅带来小幅变化。', '',
'small/batch64的逐帧时钟检查有明显开销：取消该检查但保留帧/字节上限，三次中位数从3128691提升到3626816 msg/s（+15.9%），接近同组旧版3637912。禁用全部预算并未在该档位更快，说明调度/批次交互不宜用简单相加的成本模型解释。', '',
'small/batch256包含多项成本：禁用预算+42.6%，取消逐帧时钟+20.9%，按连续批次保活codec+16.0%；第二组取消回调快照+23.5%、取消SendCmd连接保活+21.3%、取消缓冲移动+12.4%。每个单因素都不足以恢复旧版全部吞吐，且这些提升不能相加。固定预算和逐包防护叠加破坏了批量摊销收益。', '',
'公平性、异常回滚、codec/回调/缓冲保活都有明确正确性目的，不应直接回退已证实必要的安全修复。可以优化的是预算粒度、时间检查频率、内部调用的生命周期保护作用域、真正使用中间缓冲时才建立借用，以及读空后避免无效自唤醒；所有候选须重新验证重入安全、丢唤醒和多连接尾延迟。第一阶段只测3档/3次，第二阶段亦如此，属于归因线索而非最终优化验收。']
text+=['','## 代码证据与判断','',
'当前 DrainBudget::Consume 在64帧、256KiB或2ms任一条件达到时停止当前Drain；DrainRing预算耗尽后无条件WakeReader，并返回EventLoop。因此small/batch256无法按旧版本一次连续处理完批次；huge1m一帧即超过256KiB，一帧处理后就让出loop。更多CommitRead、事件分发、本地eventfd通知与批次对象初始化会减少批量收益。',
'',
'Consume在未提前命中帧/字节上限时每帧查询steady_clock，通常会增加vDSO时钟调用开销；源码中的std::chrono::steady_clock::now并不意味着必然发生内核系统调用。逐帧复制shared_ptr保活codec、普通SendCmd保活连接/codec、StableCallback保活回调也带来引用计数操作。这些是已复现重入释放后使用等问题的安全保护，不能单纯删除以追求性能。',
'',
'这是诊断测试，不是对改动后的安全、公平性和多客户端尾延迟的完整验证。no_budget会改变公平性契约，no_frame_clock会取消时间预算，均不能直接当作生产修复。更合理的方向是可配置的帧/字节预算、按批或分段检查时间、保持回调期间生命周期安全的批次保活，以及避免对已读空环发送额外本地唤醒。']
(out/'README.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text))
