#!/usr/bin/env python3
"""Aggregate local compare output or copied gate-side deployment directories."""
import argparse
import json
from pathlib import Path
import statistics as st
import sys


def expected_prefix(row):
    state = row['seed'] or 1
    mask = (1 << 64) - 1
    result = 14695981039346656037
    span = row['batch_max'] - row['batch_min'] + 1
    remaining = row.get('finite_messages', 0)
    for _ in range(row['prefix_count']):
        state ^= state >> 12; state ^= (state << 25) & mask; state ^= state >> 27; state &= mask
        n = row['batch_min'] + ((state * 2685821657736338717) & mask) % span
        if row.get('finite_messages', 0):
            n = min(n, remaining); remaining -= n
        result = ((result ^ n) * 1099511628211) & mask
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', nargs='+'); p.add_argument('--output', required=True)
    args = p.parse_args()
    files = []
    for directory in args.input:
        for path in Path(directory).rglob('status.json'):
            if json.loads(path.read_text()).get('state') == 'failed':
                print('Input includes failed runs; do not select only successful samples', file=sys.stderr); return 2
    for directory in args.input:
        files.extend(sorted(Path(directory).rglob('sample.json')))
    data = [json.loads(f.read_text()) for f in files]
    data = [x for x in data if not x.get('smoke')]
    if not data:
        print('No formal gate samples', file=sys.stderr); return 2
    identities = [(x['case'], x['round'], x['mode']) for x in data]
    if len(set(identities)) != len(identities):
        print('Duplicate case/round/mode; provide separate correctly tagged rounds', file=sys.stderr); return 2
    if len({x.get('codec', 'raw') for x in data}) != 1:
        print('Mixed codecs', file=sys.stderr); return 2
    summary = []
    cases = list(dict.fromkeys(x['case'] for x in data))
    cases.sort(key=lambda name: next((x['bytes'],x['batch_max']) for x in data if x['case']==name))
    for case in cases:
        runs = [x for x in data if x['case'] == case]
        modes = sorted(set(x['mode'] for x in runs))
        if 'uds' not in modes or len(modes) < 2:
            print('Each case needs UDS and at least one SHM mode', file=sys.stderr); return 2
        repeats = sorted(set(x['round'] for x in runs))
        pairs = []
        for repeat in repeats:
            pair = {x['mode']:x for x in runs if x['round'] == repeat}
            if set(pair) != set(modes) or len({x['seed'] for x in pair.values()}) != 1:
                print('Unpaired modes/seeds', file=sys.stderr); return 2
            if any(expected_prefix(x) != x['prefix_hash'] for x in pair.values()):
                print('Invalid measured random prefix', file=sys.stderr); return 2
            pairs.append(pair)
        if len({json.dumps(x.get('config',{}),sort_keys=True) for x in runs}) != 1 or len({(x['topology'],x['hostname']) for x in runs}) != 1:
            print('Mismatched topology/host/TCP/test configuration', file=sys.stderr); return 2
        first = runs[0]
        row = dict(case=case, bytes=first['bytes'], batch_min=first['batch_min'], batch_max=first['batch_max'],
                   codec=first.get('codec','raw'), application_payload_bytes=first.get('application_payload_bytes',first['bytes']),
                   rounds=len(repeats), topology=sorted(set(x['topology'] for x in runs)))
        if len({x['binary_sha256'] for x in runs}) != 1:
            print('Mismatched gate binary fingerprints', file=sys.stderr); return 2
        for mode in modes:
            values = [x for x in runs if x['mode'] == mode]
            row[mode] = dict(rpc_s=st.median(x['rpc_s'] for x in values),
                duplex_payload_mib_s=st.median(x['duplex_payload_mib_s'] for x in values),
                duplex_tcp_framed_mib_s=st.median(x['duplex_tcp_framed_mib_s'] for x in values),
                rpc_range=[min(x['rpc_s'] for x in values),max(x['rpc_s'] for x in values)],
                min_samples=min(x['samples'] for x in values),
                **{k+'_us':st.median(x[k+'_ns']/1000 for x in values) for k in ['p50','p99','p999']})
            if row['codec'] == 'protobuf':
                row[mode]['duplex_protobuf_mib_s'] = st.median(x['duplex_protobuf_mib_s'] for x in values)
            if mode != 'uds':
                gains = [(q[mode]['rpc_s']/q['uds']['rpc_s']-1)*100 for q in pairs]
                row[mode]['rpc_gain_range_pct'] = [min(gains),max(gains)]
                row[mode]['negative_rpc_pairs'] = sum(x<0 for x in gains)
                row[mode]['rpc_gain_pct'] = st.median((q[mode]['rpc_s']/q['uds']['rpc_s']-1)*100 for q in pairs)
                row[mode]['p50_reduction_pct'] = st.median((1-q[mode]['p50_ns']/q['uds']['p50_ns'])*100 for q in pairs)
        summary.append(row)
    output = Path(args.output); output.mkdir(parents=True, exist_ok=True)
    variation = []
    for row in summary:
        for mode in ['uds','shm-eventfd','shm-busy']:
            if mode not in row: continue
            low,high = row[mode]['rpc_range']
            if high/low > 1.2:
                variation.append(dict(case=row['case'],mode=mode,min_rpc_s=low,max_rpc_s=high,max_vs_min_pct=(high/low-1)*100))
    (output/'variation.json').write_text(json.dumps(variation,indent=2)+'\n')
    (output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    rows=['| 场景 | 本机段模式 | RPC/s | P50/P99.9 RTT µs | 双向消息 MiB/s | 含应用帧头TCP MiB/s | 相对UDS吞吐提升 |',
          '|---|---|---:|---:|---:|---:|---:|']
    for row in summary:
        for mode in ['uds','shm-eventfd','shm-busy']:
            if mode not in row:continue
            x=row[mode];gain='—' if mode=='uds' else f"{x['rpc_gain_pct']:+.1f}%"
            rows.append(f"| {row['bytes']}B / {row['batch_min']}–{row['batch_max']} | {mode} | {x['rpc_s']:,.0f} | {x['p50_us']:.2f}/{x['p999_us']:.2f} | {x.get('duplex_protobuf_mib_s',x['duplex_payload_mib_s']):.3f} | {x['duplex_tcp_framed_mib_s']:.3f} | {gain} |")
    (output/'comparison.md').write_text('\n'.join(rows)+'\n')
    print('\n'.join(rows));return 0


if __name__ == '__main__':
    sys.exit(main())
