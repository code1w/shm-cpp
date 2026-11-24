#!/usr/bin/env python3
"""Analyze paired random-batch runs; preserve gains and losses for both SHM modes."""
import json
from pathlib import Path
import statistics as st
import sys


def main():
    out = Path(sys.argv[1])
    meta = json.loads((out / 'completed.json').read_text())
    samples = json.loads((out / 'samples.json').read_text())
    if not meta['completed'] or meta['smoke'] or len(samples) != meta['records']:
        return 2
    summary = []
    for case in dict.fromkeys(x['case'] for x in samples):
        pairs = []
        for repeat in range(1, meta['rounds'] + 1):
            entries = [x for x in samples if x['case'] == case and x['round'] == repeat]
            pair = {x['mode']: x for x in entries}
            if len(entries) != 3 or set(pair) != set(meta['modes']) or len({x['seed'] for x in entries}) != 1:
                return 2
            if len({x['batch_stats']['prefix_hash'] for x in entries}) != 1:
                return 2
            pairs.append(pair)
        first = pairs[0]['socket']
        row = dict(case=case, bytes=first['bytes'], profile=first['profile'], minimum=first['minimum'], maximum=first['maximum'])
        for mode in meta['modes']:
            runs = [p[mode] for p in pairs]
            row[mode] = dict(rpc_s=st.median(x['msg_s'] for x in runs),
                payload_mib_s=st.median(x['msg_s'] * x['bytes'] / 1048576 for x in runs),
                rpc_range=[min(x['msg_s'] for x in runs), max(x['msg_s'] for x in runs)],
                mean_batch=st.median(x['batch_stats']['messages'] / x['batch_stats']['batches'] for x in runs),
                min_samples=min(x['samples'] for x in runs),
                **{k + '_us': st.median(x[k + '_ns'] / 1000 for x in runs) for k in ['p50', 'p99', 'p999']})
        for mode in ['shm-eventfd', 'shm-busy']:
            row[mode]['paired_rpc_gain_pct'] = st.median((p[mode]['msg_s'] / p['socket']['msg_s'] - 1) * 100 for p in pairs)
            row[mode]['paired_p50_reduction_pct'] = st.median((1 - p[mode]['p50_ns'] / p['socket']['p50_ns']) * 100 for p in pairs)
        row['busy_vs_eventfd_gain_pct'] = st.median((p['shm-busy']['msg_s'] / p['shm-eventfd']['msg_s'] - 1) * 100 for p in pairs)
        summary.append(row)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    throughput = ['| 消息体 / 随机batch | Socket RPC/s | SHM eventfd RPC/s | eventfd提升 | SHM busy RPC/s | busy提升 |',
                  '|---|---:|---:|---:|---:|---:|']
    rtt = ['| 消息体 / 随机batch | Socket P50/P99.9 µs | SHM eventfd P50/P99.9 µs | SHM busy P50/P99.9 µs |',
           '|---|---:|---:|---:|']
    for row in summary:
        a, b, c = row['socket'], row['shm-eventfd'], row['shm-busy']
        label = f"{row['bytes']}B / {row['minimum']}–{row['maximum']}"
        throughput.append(f"| {label} | {a['rpc_s']:,.0f} | {b['rpc_s']:,.0f} | {b['paired_rpc_gain_pct']:+.1f}% | {c['rpc_s']:,.0f} | {c['paired_rpc_gain_pct']:+.1f}% |")
        rtt.append(f"| {label} | {a['p50_us']:.2f}/{a['p999_us']:.2f} | {b['p50_us']:.2f}/{b['p999_us']:.2f} | {c['p50_us']:.2f}/{c['p999_us']:.2f} |")
    (out / 'throughput_table.md').write_text('\n'.join(throughput) + '\n')
    (out / 'rtt_table.md').write_text('\n'.join(rtt) + '\n')
    print('\n'.join(throughput))
    return 0


if __name__ == '__main__':
    sys.exit(main())
