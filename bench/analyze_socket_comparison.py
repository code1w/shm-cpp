#!/usr/bin/env python3
"""Pair each case/round before taking ratios; exclude calibration/smoke."""
import json
from pathlib import Path
import statistics as st
import sys
out=Path(sys.argv[1]);data=json.loads((out/'samples.json').read_text())
meta=json.loads((out/'completed.json').read_text())
assert len(data)==meta['records'] and meta['completed'] and not meta['smoke']
labels=list(dict.fromkeys(x['case'] for x in data))
summary=[]
for case in labels:
 pairs=[]
 for round in range(1,4):
  entries=[x for x in data if x['case']==case and x['round']==round]
  assert len(entries)==2
  pair={x['side']:x for x in entries};assert set(pair)=={'shm','socket'}
  assert pair['shm']['bytes']==pair['socket']['bytes'] and pair['shm']['batch']==pair['socket']['batch']
  pairs.append(pair)
 row={'case':case,'bytes':pairs[0]['shm']['bytes'],'batch':pairs[0]['shm']['batch'],
      'paired_rpc_ratio_median':st.median(p['shm']['msg_s']/p['socket']['msg_s'] for p in pairs),
      'paired_rpc_gain_pct':st.median((p['shm']['msg_s']/p['socket']['msg_s']-1)*100 for p in pairs),
      'paired_p50_reduction_pct':st.median((1-p['shm']['p50_ns']/p['socket']['p50_ns'])*100 for p in pairs),
      'paired_p999_reduction_pct':st.median((1-p['shm']['p999_ns']/p['socket']['p999_ns'])*100 for p in pairs)}
 for side in ['shm','socket']:
  runs=[p[side] for p in pairs]
  row[side]={'rpc_s':st.median(x['msg_s'] for x in runs),
             'payload_mib_s':st.median(x['msg_s']*x['bytes']/1048576 for x in runs),
             **{key+'_us':st.median(x[key+'_ns']/1000 for x in runs) for key in ['p50','p99','p999']},
             'max_us':max(x['max_ns']/1000 for x in runs),
             'min_samples':min(x['samples'] for x in runs),
             'rpc_s_range':[min(x['msg_s'] for x in runs),max(x['msg_s'] for x in runs)]}
 summary.append(row)
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
throughput=['| 场景 | SHM RPC/s | Socket RPC/s | 配对吞吐倍率 | SHM吞吐提升 |','|---|---:|---:|---:|---:|']
latency=['| 场景 | SHM P50 µs | Socket P50 µs | SHM P99 µs | Socket P99 µs | SHM P99.9 µs | Socket P99.9 µs |','|---|---:|---:|---:|---:|---:|---:|']
for x in summary:
 a,b=x['shm'],x['socket']
 throughput.append(f"| {x['case']} | {a['rpc_s']:,.0f} | {b['rpc_s']:,.0f} | {x['paired_rpc_ratio_median']:.2f}× | {x['paired_rpc_gain_pct']:+.1f}% |")
 latency.append(f"| {x['case']} | {a['p50_us']:.2f} | {b['p50_us']:.2f} | {a['p99_us']:.2f} | {b['p99_us']:.2f} | {a['p999_us']:.2f} | {b['p999_us']:.2f} |")
(out/'throughput_table.md').write_text('\n'.join(throughput)+'\n')
(out/'rtt_table.md').write_text('\n'.join(latency)+'\n')
print('\n'.join(throughput));print('\n'.join(latency))
