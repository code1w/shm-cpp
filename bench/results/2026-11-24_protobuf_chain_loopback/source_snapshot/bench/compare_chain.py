#!/usr/bin/env python3
"""Four real processes on one machine, TCP loopback verification/comparison."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
from chain_run_support import MODES, Runner, executable, gate_metric, sample, validate_all

FULL = [16, 128, 256, 512, 1024, 2048, 3072, 4096, 8192, 16384, 32768, 65536, 131072, 524288, 1048576]


def run(args, output, size, high, mode, repeat, seed):
    directory = Path(tempfile.mkdtemp(prefix='chain-'))
    world_path, gate_path = str(directory / 'world.sock'), str(directory / 'gate.sock')
    runner = Runner(args.binary, output)
    common = ['--mode', mode]
    runner.launch('world', args.cpus[3], common + ['--local', world_path])
    valid = runner.ready('world') is not None
    if valid:
        runner.launch('world-sidecar', args.cpus[2], common + ['--local', world_path, '--tcp-address', '127.0.0.1', '--tcp-port', 0])
        ready = runner.ready('world-sidecar')
        valid = ready is not None
    if valid:
        runner.launch('gate-sidecar', args.cpus[1], common + ['--local', gate_path, '--tcp-address', '127.0.0.1', '--tcp-port', ready['tcp_port']])
        valid = runner.ready('gate-sidecar') is not None
    if valid:
        runner.launch('gate', args.cpus[0], common + ['--local', gate_path, '--bytes', size, '--batch-min', 1,
            '--batch-max', high, '--seed', seed, '--stride', 1 if args.smoke or size >= 65536 else 64,
            '--seconds', args.seconds, '--warmup', 0 if args.smoke else 1, '--messages', args.messages if args.smoke else 0])
        valid = runner.ready('gate') is not None and runner.wait(args.seconds + 45)
    finals = runner.finals() if valid else None
    metric = gate_metric(output) if valid else None
    valid = validate_all(metric, finals)
    result = sample(metric, mode, repeat, finals, args.binary, 'single-host-tcp-loopback', args.smoke,
        args.messages if args.smoke else 0, dict(cpus=args.cpus, tcp_address='127.0.0.1', tcp_buffer=0,
        seconds=args.seconds, warmup=0 if args.smoke else 1)) if valid else None
    runner.stop()
    for path in [world_path, gate_path]:
        if Path(path).exists():
            Path(path).unlink()
    directory.rmdir()
    if result:
        (output / 'sample.json').write_text(json.dumps(result, indent=2) + '\n')
        print(result['case'], mode, 'RPC/s', round(result['rpc_s']), 'P50 us', result['p50_ns'] / 1000,
              'duplex payload MiB/s', round(result['duplex_payload_mib_s'], 3), flush=True)
    else:
        print('FAILED', size, high, mode, file=sys.stderr, flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary'); p.add_argument('output')
    p.add_argument('--sizes', type=int, nargs='+', default=[16, 4096, 65536, 1048576])
    p.add_argument('--full', action='store_true')
    p.add_argument('--cpus', type=int, nargs=4, default=[1, 3, 5, 7], help='gate, gate-sidecar, world-sidecar, world')
    p.add_argument('--seconds', type=float, default=5); p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--smoke', action='store_true'); p.add_argument('--messages', type=int, default=257)
    args = p.parse_args()
    if args.full:
        args.sizes = FULL
    if not executable(args.binary) or len(set(args.cpus)) != 4 or args.seconds <= 0 or args.rounds <= 0 or args.messages <= 0 or any(x not in FULL for x in args.sizes):
        return 2
    args.binary = str(Path(args.binary).resolve()); out = Path(args.output).resolve()
    if (out / 'samples.json').exists():
        print('Choose a fresh output directory', file=sys.stderr); return 2
    out.mkdir(parents=True, exist_ok=True)
    cases = []
    for size in args.sizes:
        cap = min(256, 2 * 1024 * 1024 // size)
        for high in sorted({min(8, cap), min(64, cap)}):
            cases.append((size, high))
    samples = []
    rounds = 1 if args.smoke else args.rounds
    total = len(cases) * rounds * len(MODES)
    for repeat in range(rounds):
        order = list(enumerate(cases))
        if repeat % 2:
            order.reverse()
        for index, (size, high) in order:
            seed = 0x7654321 + index * 4099 + repeat * 65537
            group = []
            for position in range(3):
                mode = MODES[(index + repeat + position) % 3]
                folder = out / f'{size}B-b1-{high}-r{repeat+1}-{mode}'
                row = run(args, folder, size, high, mode, repeat + 1, seed)
                if row is None:
                    (out / 'status.json').write_text(json.dumps(dict(state='failed', completed=len(samples), total=total))+'\n')
                    return 1
                samples.append(row); group.append(row)
                (out / 'samples.json').write_text(json.dumps(samples, indent=2)+'\n')
                (out / 'status.json').write_text(json.dumps(dict(state='running', completed=len(samples), total=total))+'\n')
            if len({(x['prefix_count'],x['prefix_hash']) for x in group}) != 1:
                print('Random prefix mismatch', file=sys.stderr); return 1
    (out / 'completed.json').write_text(json.dumps(dict(completed=True, records=len(samples), rounds=rounds,
        smoke=args.smoke, topology='single-host-tcp-loopback', cpus=args.cpus, seconds=args.seconds))+'\n')
    (out / 'status.json').write_text(json.dumps(dict(state='passed', completed=len(samples), total=total))+'\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
