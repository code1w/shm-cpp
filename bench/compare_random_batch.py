#!/usr/bin/env python3
"""Serial, paired three-transport comparison with reproducible variable batches."""
import argparse
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

SIZES = [16, 128, 256, 512, 1024, 2048, 3072, 4096, 8192, 16384, 32768, 65536, 131072, 524288, 1048576]
MODES = ['shm-eventfd', 'shm-busy', 'socket']
interrupted = False


def stop_requested(signum, frame):
    global interrupted
    interrupted = True


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines(keepends=True)
            if line.startswith('{') and line.endswith('\n')]


def wait_child(process, deadline):
    while process.poll() is None and not interrupted and time.monotonic() < deadline:
        time.sleep(.02)
    if process.poll() is None:
        process.kill()
        process.wait()
        return False
    return process.returncode == 0


def run_case(args, out, size, low, high, profile, mode, repeat, position, seed):
    directory = Path(tempfile.mkdtemp(prefix='shm-random-ab-'))
    path = str(directory / 'ipc.sock')
    label = f'{size}B-{profile}-r{repeat}-{mode}'
    handles, children = [], []
    flags = ['--transport', 'socket' if mode == 'socket' else 'shm', '--busy-poll', int(mode == 'shm-busy'),
             '--bytes', size, '--batch', high, '--batch-min', low, '--seed', seed, '--single-type', 1]

    def launch(role, extra):
        logfile = out / (label + '-' + role + '.jsonl')
        log = logfile.open('w')
        err = (out / (label + '-' + role + '.stderr')).open('w')
        handles.extend([log, err])
        command = ['taskset', '-c', str(args.server_cpu if role == 'server' else args.client_cpu),
                   args.binary, role, path] + list(map(str, flags + extra))
        (out / (label + '-' + role + '.command.json')).write_text(json.dumps(command) + '\n')
        process = subprocess.Popen(command, stdout=log, stderr=err)
        children.append(process)
        return process, logfile

    server, server_log = launch('server', [])
    deadline = time.monotonic() + 8
    ready = False
    while server.poll() is None and not interrupted and time.monotonic() < deadline:
        if any(x['role'] == 'ready' for x in records(server_log)):
            ready = True
            break
        time.sleep(.02)
    valid, result = False, None
    if ready:
        stride = 64 if size <= 4096 else 16 if size < 65536 else 1
        client, client_log = launch('client', ['--stride', stride, '--warmup', 0 if args.smoke else 1,
                                              '--seconds', .3 if args.smoke else args.seconds])
        valid = wait_child(client, time.monotonic() + args.seconds + 20)
        if valid:
            data = records(client_log)
            metrics = [x for x in data if x['role'] == 'result']
            batches = [x for x in data if x['role'] == 'batch_stats']
            valid = bool(metrics and batches and data[-1]['role'] == 'client_final')
            if valid:
                metric, stats, final = metrics[0], batches[0], data[-1]
                counts = stats['counts']
                valid = (final['completed'] and not final['failed'] and final['base_fds'] == final['final_fds']
                         and final['ring_maps'] == 0 and metric['errors'] == 0 and metric['samples'] > 0
                         and sum(counts) == stats['batches']
                         and sum(i * n for i, n in enumerate(counts)) == metric['messages'] == stats['messages']
                         and all(n == 0 for i, n in enumerate(counts) if i < low or i > high))
                result = dict(metric, batch_stats=stats, mode=mode, case=f'{size}B-{profile}', profile=profile,
                              round=repeat, position=position, minimum=low, maximum=high, seed=seed)
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
        valid = wait_child(server, time.monotonic() + 5) and valid
        if valid:
            data = records(server_log)
            final = data[-1]
            valid = (final['role'] == 'server_final' and final['opened'] == final['closed']
                     and final['base_fds'] == final['final_fds']
                     and all(x.get('send_failures', 0) == 0 for x in data))
    for child in children:
        if child.poll() is None:
            child.kill()
            child.wait()
    for handle in handles:
        handle.close()
    if Path(path).exists():
        Path(path).unlink()
    directory.rmdir()
    if not valid:
        print('FAILED', label, file=sys.stderr, flush=True)
        return None
    print(label, f"batch={low}..{high}", 'RPC/s', round(result['msg_s']),
          'P50 us', result['p50_ns'] / 1000, flush=True)
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('binary'); p.add_argument('output')
    p.add_argument('--smoke', action='store_true')
    p.add_argument('--seconds', type=float, default=5)
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--server-cpu', type=int, default=1)
    p.add_argument('--client-cpu', type=int, default=3)
    p.add_argument('--sizes', type=int, nargs='+', default=SIZES)
    args = p.parse_args()
    if args.server_cpu == args.client_cpu or args.seconds <= 0 or args.rounds <= 0 or any(s not in SIZES for s in args.sizes):
        print('Invalid options', file=sys.stderr)
        return 2
    args.binary = str(Path(args.binary).resolve())
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'samples.json').exists():
        print('Output already contains samples; choose a new directory', file=sys.stderr)
        return 2
    signal.signal(signal.SIGINT, stop_requested); signal.signal(signal.SIGTERM, stop_requested)
    cases = []
    for size in args.sizes:
        cap = 64 if size <= 32768 else 32 if size <= 65536 else 16 if size <= 131072 else 4
        cap = min(cap, 1 << ((5 * 1024 * 1024 // (size + 4)).bit_length() - 1))
        for profile, high in [('small', min(8, cap // 2)), ('burst', cap)]:
            cases.append((size, 1, high, profile))
    samples = []
    total = len(cases) * len(MODES) * (1 if args.smoke else args.rounds)
    (out / 'binary.sha256').write_text(hashlib.sha256(Path(args.binary).read_bytes()).hexdigest() + '  ' + args.binary + '\n')
    for repeat in range(1 if args.smoke else args.rounds):
        order = list(enumerate(cases))
        if repeat % 2:
            order.reverse()
        for index, (size, low, high, profile) in order:
            seed = 0x13579BDF + index * 4099 + repeat * 65537
            block = []
            for position in range(3):
                mode = MODES[(index + repeat + position) % 3]
                result = run_case(args, out, size, low, high, profile, mode, repeat + 1, position, seed)
                if result is None:
                    (out / 'status.json').write_text(json.dumps(dict(state='failed', completed=len(samples), total=total)) + '\n')
                    return 1
                samples.append(result); block.append(result)
                (out / 'samples.json').write_text(json.dumps(samples, indent=2) + '\n')
                (out / 'status.json').write_text(json.dumps(dict(state='running', completed=len(samples), total=total, latest=result['case'])) + '\n')
            # Formal runs must start from an identical measured random prefix.
            if not args.smoke and (any(x['batch_stats']['prefix_count'] != 256 for x in block)
                                   or len({x['batch_stats']['prefix_hash'] for x in block}) != 1):
                print('Random prefix mismatch', block[0]['case'], file=sys.stderr)
                return 1
    (out / 'completed.json').write_text(json.dumps(dict(completed=True, smoke=args.smoke, records=len(samples),
        rounds=1 if args.smoke else args.rounds, seconds=.3 if args.smoke else args.seconds, modes=MODES,
        sizes=args.sizes, server_cpu=args.server_cpu, client_cpu=args.client_cpu, distribution='uniform_batches',
        seed_reset_after_warmup=True, prefix_length=256, socket_coalesced=True)) + '\n')
    (out / 'status.json').write_text(json.dumps(dict(state='passed', completed=len(samples), total=total)) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
