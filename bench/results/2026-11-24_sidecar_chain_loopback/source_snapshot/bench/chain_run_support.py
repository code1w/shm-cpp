"""Process ownership, readiness and four-role validation for chain benchmarks."""
import atexit
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

MODES = ['uds', 'shm-eventfd', 'shm-busy']


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines(keepends=True)
            if line.startswith('{') and line.endswith('\n')]


class Runner:
    def __init__(self, binary, output):
        self.binary = str(Path(binary).resolve())
        self.out = Path(output)
        self.out.mkdir(parents=True, exist_ok=True)
        self.children = {}
        self.handles = []
        self.interrupted = False
        atexit.register(self.stop)
        signal.signal(signal.SIGINT, self.request_stop)
        signal.signal(signal.SIGTERM, self.request_stop)

    def request_stop(self, signum, frame):
        self.interrupted = True

    def launch(self, role, cpu, flags):
        logfile = self.out / (role + '.jsonl')
        log = logfile.open('w')
        err = (self.out / (role + '.stderr')).open('w')
        self.handles.extend([log, err])
        command = ['taskset', '-c', str(cpu), self.binary, role] + list(map(str, flags))
        (self.out / (role + '.command.json')).write_text(json.dumps(command) + '\n')
        self.children[role] = subprocess.Popen(command, stdout=log, stderr=err)
        return self.children[role]

    def ready(self, role):
        deadline = time.monotonic() + 15
        process = self.children[role]
        while not self.interrupted and time.monotonic() < deadline:
            matches = [x for x in records(self.out / (role + '.jsonl')) if x['role'] == 'ready']
            if matches:
                return matches[-1]
            if process.poll() is not None:
                return None
            time.sleep(.02)
        return None

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while not self.interrupted and (deadline is None or time.monotonic() < deadline):
            codes = [p.poll() for p in self.children.values()]
            if any(code is not None and code != 0 for code in codes):
                return False
            if all(code == 0 for code in codes):
                return True
            time.sleep(.02)
        return False

    def stop(self):
        for process in self.children.values():
            if process.poll() is None:
                process.terminate()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and any(p.poll() is None for p in self.children.values()):
            time.sleep(.02)
        for process in self.children.values():
            if process.poll() is None:
                process.kill()
                process.wait()
        for handle in self.handles:
            if not handle.closed:
                handle.close()
        self.children.clear()
        atexit.unregister(self.stop)

    def finals(self):
        result = {}
        for role in self.children:
            rows = records(self.out / (role + '.jsonl'))
            final = rows[-1] if rows else {}
            if final.get('role') != 'chain_final' or final.get('error') != 'ok' or final['base_fds'] != final['final_fds']:
                return None
            result[role] = final
        return result


def gate_metric(output):
    values = [x for x in records(Path(output) / 'gate.jsonl') if x['role'] == 'result']
    return values[0] if len(values) == 1 else None


def validate_all(metric, finals):
    if not metric or not finals or len(finals) != 4:
        return False
    gate = finals['gate']
    count = gate['sent']
    wire = count * (metric['bytes'] + 16)
    if not gate['completed'] or count != gate['received'] or metric['messages'] != metric['batch_messages'] or metric['samples'] == 0:
        return False
    for role, row in finals.items():
        if row['local_rx'] != count or row['local_tx'] != count:
            return False
        if role in ['gate-sidecar', 'world-sidecar']:
            if row['tcp_rx_frames'] != count or row['tcp_tx_frames'] != count or row['tcp_tx_bytes'] != wire or row['tcp_rx_bytes'] != wire:
                return False
    return True


def sample(metric, mode, repeat, finals, binary, topology, smoke=False, finite=0, config=None):
    return dict(metric, mode=mode, round=repeat, case=f"{metric['bytes']}B-b{metric['batch_min']}-{metric['batch_max']}",
                actor_finals=finals, binary_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
                topology=topology, hostname=socket.gethostname(), smoke=smoke, finite_messages=finite, config=config or {})


def executable(binary):
    return Path(binary).is_file() and os.access(binary, os.X_OK)
