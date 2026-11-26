#!/usr/bin/env python3
"""Launch only the two processes belonging to this host; no SSH/deployment."""
import argparse
import json
from pathlib import Path
import socket
import sys
import tempfile
from chain_run_support import MODES, Runner, executable, gate_metric, sample


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('side', choices=['gate', 'world'])
    p.add_argument('--binary', default='./build/bench_chain')
    p.add_argument('--mode', choices=MODES, required=True)
    p.add_argument('--tcp-address', required=True, help='World bind address, or gate-side TCP destination IPv4')
    p.add_argument('--tcp-port', type=int, default=19001)
    p.add_argument('--tcp-buffer', type=int, default=0, help='0 preserves TCP autotuning')
    p.add_argument('--output', required=True)
    p.add_argument('--local', help='Optional local socket path; defaults to a private temporary directory')
    p.add_argument('--app-cpu', type=int, default=1); p.add_argument('--sidecar-cpu', type=int, default=3)
    p.add_argument('--bytes', type=int, default=16)
    p.add_argument('--batch-min', type=int, default=1); p.add_argument('--batch-max', type=int, default=8)
    p.add_argument('--seed', type=int, default=1); p.add_argument('--round', type=int, default=1)
    p.add_argument('--stride', type=int, default=1)
    p.add_argument('--seconds', type=float, default=10); p.add_argument('--warmup', type=float, default=2)
    p.add_argument('--timeout', type=float, default=30); p.add_argument('--messages', type=int, default=0)
    args = p.parse_args()
    if not executable(args.binary) or args.app_cpu == args.sidecar_cpu or not (0 < args.tcp_port < 65536):
        return 2
    output = Path(args.output).resolve()
    if (output / 'run.json').exists():
        print('Choose a fresh output directory', file=sys.stderr); return 2
    directory = Path(tempfile.mkdtemp(prefix='chain-host-')) if args.local is None else None
    local = str(directory / 'ipc.sock') if directory else args.local
    runner = Runner(args.binary, output)
    meta = dict(vars(args), local=local, hostname=socket.gethostname(), topology='user-managed-deployment')
    (output / 'run.json').write_text(json.dumps(meta, indent=2) + '\n')
    common = ['--mode', args.mode, '--local', local, '--timeout', args.timeout]
    net = ['--tcp-address', args.tcp_address, '--tcp-port', args.tcp_port, '--tcp-buffer', args.tcp_buffer]
    valid = True
    if args.side == 'world':
        runner.launch('world', args.app_cpu, common)
        valid = runner.ready('world') is not None
        if valid:
            runner.launch('world-sidecar', args.sidecar_cpu, common + net)
            valid = runner.ready('world-sidecar') is not None
    else:
        runner.launch('gate-sidecar', args.sidecar_cpu, common + net)
        valid = runner.ready('gate-sidecar') is not None
        if valid:
            gate = common + ['--bytes', args.bytes, '--batch-min', args.batch_min, '--batch-max', args.batch_max,
                '--seed', args.seed, '--stride', args.stride, '--seconds', args.seconds, '--warmup', args.warmup,
                '--messages', args.messages]
            runner.launch('gate', args.app_cpu, gate)
            valid = runner.ready('gate') is not None
    if valid:
        print(f"READY {args.side} pair, mode={args.mode}, logs={output}", flush=True)
        valid = runner.wait() # World may intentionally wait for a manually started gate.
    finals = runner.finals() if valid else None
    valid = bool(finals) and valid
    metric = gate_metric(output) if valid and args.side == 'gate' else None
    if args.side == 'gate':
        valid = valid and bool(metric) and finals['gate']['completed'] and metric['samples'] > 0
        if valid:
            row = sample(metric, args.mode, args.round, finals, str(Path(args.binary).resolve()), 'user-managed-deployment',
                finite=args.messages, config=dict(cpus=[args.app_cpu,args.sidecar_cpu],tcp_address=args.tcp_address,
                tcp_port=args.tcp_port,tcp_buffer=args.tcp_buffer,seconds=args.seconds,warmup=args.warmup,stride=args.stride))
            (output / 'sample.json').write_text(json.dumps(row, indent=2) + '\n')
            print(json.dumps(metric), flush=True)
    (output / 'status.json').write_text(json.dumps(dict(state='passed' if valid else 'failed', side=args.side)) + '\n')
    runner.stop()
    if directory:
        if Path(local).exists():
            Path(local).unlink()
        directory.rmdir()
    return 0 if valid else 1


if __name__ == '__main__':
    sys.exit(main())
