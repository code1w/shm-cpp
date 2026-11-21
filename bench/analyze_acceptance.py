#!/usr/bin/env python3
"""Summarize measured acceptance evidence without inventing a latency SLA."""
import csv
import json
from pathlib import Path
import re
import statistics as st
import sys
out=Path(sys.argv[1]);metrics=json.loads((out/'metrics.json').read_text())
performance={}
for x in metrics:
    m=re.fullmatch(r'(.+)-r(\d+)',x['case'])
    if m:performance.setdefault(m[1],[]).append(x)
summary=[]
for name,runs in performance.items():
    assert len(runs)==3
    summary.append({'case':name,'runs':len(runs),'rpc_s_median':st.median(x['msg_s'] for x in runs),
        'p50_us_median':st.median(x['p50_ns']/1000 for x in runs),
        'p99_us_median':st.median(x['p99_ns']/1000 for x in runs),
        'p999_us_median':st.median(x['p999_ns']/1000 for x in runs),
        'max_us':max(x['max_ns']/1000 for x in runs),'min_samples':min(x['samples'] for x in runs),
        'total_samples':sum(x['samples'] for x in runs),'rpc_s_range':[min(x['msg_s'] for x in runs),max(x['msg_s'] for x in runs)]})
resources=list(csv.DictReader((out/'resources.csv').open()))
soak=[]
for role in ['server','client']:
    values=[x for x in resources if x['phase']=='soak' and x['role']==role and float(x['elapsed'])>=30]
    assert len(values)>100
    first,last=values[:60],values[-60:]
    soak.append({'role':role,'samples':len(values),
        **{key+'_first_median':st.median(int(x[key]) for x in first) for key in ['rss_kib','fds','ring_maps','threads']},
        **{key+'_last_median':st.median(int(x[key]) for x in last) for key in ['rss_kib','fds','ring_maps','threads']},
        'rss_kib_range':[min(int(x['rss_kib']) for x in values),max(int(x['rss_kib']) for x in values)],
        'fd_range':[min(int(x['fds']) for x in values),max(int(x['fds']) for x in values)],
        'maps_range':[min(int(x['ring_maps']) for x in values),max(int(x['ring_maps']) for x in values)]})
assert all(x['fds_first_median']==x['fds_last_median'] and x['ring_maps_first_median']==x['ring_maps_last_median'] for x in soak)
result={'performance':summary,'resource_soak':soak,'soak_result':[x for x in metrics if x['case']=='soak'][0],
    'faults':json.loads((out/'faults.json').read_text()),'latency_scope':'closed-loop admitted RPC RTT, histogram upper bounds, no latency SLA asserted'}
(out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
lines=['| 场景 | RPC/s 中位数 | P50 µs | P99 µs | P99.9 µs | 每轮最少样本 |','|---|---:|---:|---:|---:|---:|']
for x in summary:lines.append(f"| {x['case']} | {x['rpc_s_median']:,.0f} | {x['p50_us_median']:.2f} | {x['p99_us_median']:.2f} | {x['p999_us_median']:.2f} | {x['min_samples']:,} |")
(out/'performance_table.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
for x in soak:print(x)
