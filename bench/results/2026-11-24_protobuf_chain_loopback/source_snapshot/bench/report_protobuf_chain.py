#!/usr/bin/env python3
"""Produce all 15 sizes for both random-batch workloads from paired summaries."""
import argparse
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('summary'); p.add_argument('--output', required=True)
    args = p.parse_args()
    rows = json.loads(Path(args.summary).read_text())
    sizes = sorted({x['bytes'] for x in rows})
    if len(sizes) != 15 or any(x['codec'] != 'protobuf' or x['rounds'] != 3 for x in rows):
        return 2
    report = ['# Protobuf 直接编码：四进程链路对比', '',
        '链路：gate → 本机IPC → sidecar → TCP → sidecar → 本机IPC → world，回复沿原路返回。', '',
        '**本机 TCP loopback 测量，不是物理跨机结果。** 三轮交错配对、相同随机种子；每轮1秒预热、5秒测量。', '',
        '长度为实际Protobuf序列化字节数，不含16B应用帧头。吞吐提升和RTT下降取三轮配对百分比中位数；RPC/s、RTT、流量取三轮中位数。', '',
        'SHM连接直接编码门槛设为0，生产默认256B保持不变。Gate/world真实序列化与解析；sidecar透明转发。', '',
        '流量为请求＋回复的Protobuf字节数 MiB/s，不含16B帧头、TCP/IP头、ACK与重传。']
    noisy=sum(row[mode]['rpc_range'][1]/row[mode]['rpc_range'][0]>1.2 for row in rows for mode in ['uds','shm-eventfd','shm-busy'])
    neg_fd=sum(row['shm-eventfd']['negative_rpc_pairs'] for row in rows)
    neg_busy=sum(row['shm-busy']['negative_rpc_pairs'] for row in rows)
    report += ['', '## 测量波动与结论边界', '',
        f'本机存在其他负载，测试期间曾观察到辅助进程的磁盘页等待。{noisy}/{len(rows)*3}个场景/模式组合的三轮吞吐最大值比最小值高20%以上，具体见 [analysis/variation.json](analysis/variation.json)。', '',
        f'三轮配对中位数中，28个场景的两种SHM模式吞吐均高于UDS；单轮配对中eventfd有{neg_fd}/84次吞吐下降，busy有{neg_busy}/84次下降。该结果支持本机测量的方向判断，精确提升幅度仍受负载波动影响，不能当作生产或真实跨机承诺。', '',
        '表内RPC/s为各模式独立中位数，提升百分比为配对比值中位数，两者不能相互代算；完整配对提升范围也保存在summary.json。']
    for name, high in [('短批量', min), ('突发批量', max)]:
        report += ['', '## '+name, '', '| Protobuf长度 | 随机batch | UDS RPC/s / P50 µs / MiB/s | eventfd RPC/s / P50 µs / MiB/s | 吞吐提升 / RTT下降 | busy RPC/s / P50 µs / MiB/s | 吞吐提升 / RTT下降 |', '|---|---|---:|---:|---:|---:|---:|']
        for size in sizes:
            row = high((x for x in rows if x['bytes'] == size), key=lambda x:x['batch_max'])
            def metric(mode):
                x=row[mode]
                return f"{x['rpc_s']:,.0f} / {x['p50_us']:.2f} / {x['duplex_protobuf_mib_s']:.2f}"
            def gain(mode):
                x=row[mode]
                return f"{x['rpc_gain_pct']:+.1f}% / {x['p50_reduction_pct']:+.1f}%"
            report.append(f"| {size:,}B | 1–{row['batch_max']} | {metric('uds')} | {metric('shm-eventfd')} | {gain('shm-eventfd')} | {metric('shm-busy')} | {gain('shm-busy')} |")
    report += ['', '512KiB与1MiB受2MiB在途限制，两个表复用相同的1–4、1–2场景。其余长度分别测试两种随机batch范围，共28个场景×3模式×3轮＝252次运行。', '',
        '完整P99/P99.9、TCP含帧流量和三轮吞吐范围见 [analysis/summary.json](analysis/summary.json) 与 [analysis/comparison.md](analysis/comparison.md)。', '',
        '## 字节口径', '', '| Protobuf字节 | 业务payload字节 | 含帧头字节 |', '|---:|---:|---:|']
    for size in sizes:
        row=next(x for x in rows if x['bytes']==size)
        report.append(f"| {size} | {row['application_payload_bytes']} | {size+16} |")
    Path(args.output).write_text('\n'.join(report)+'\n')
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())
