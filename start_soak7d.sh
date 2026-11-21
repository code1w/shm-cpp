#!/usr/bin/env bash
# Build and launch the SHM soak supervisor. Default duration: seven days.
set -euo pipefail

soak_repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$soak_repo_dir"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<'HELP'
Usage: ./start_soak7d.sh [run_soak.py timing/workload options]
Defaults: 604800 seconds, 16B batch256, server CPU2, client CPU4.
Examples:
  ./start_soak7d.sh
  ./start_soak7d.sh --seconds 60
  ./start_soak7d.sh --bytes 1048576 --batch 4
Environment:
  SOAK_OUTPUT_DIR  Fresh output directory (relative to repository or absolute).
  SOAK_BUILD_JOBS Build parallelism (default 4).
The script prints status, log-following and stop commands after startup.
HELP
    exit 0
fi

for soak_tool in cmake python3 nohup taskset; do
    command -v "$soak_tool" >/dev/null || {
        printf 'Required command is missing: %s\n' "$soak_tool" >&2
        exit 1
    }
done

soak_build_dir="$soak_repo_dir/build/soak7d"
soak_output_dir="${SOAK_OUTPUT_DIR:-$soak_repo_dir/bench/results/shm_soak_7d_$(date +%Y%m%d_%H%M%S)_$$}"
soak_output_dir="$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$soak_output_dir")"
soak_console_log="${soak_output_dir}.console.log"
if [[ -e "$soak_output_dir" || -e "$soak_console_log" ]]; then
    printf 'Output already exists; choose a fresh SOAK_OUTPUT_DIR: %s\n' "$soak_output_dir" >&2
    exit 1
fi

cmake -S "$soak_repo_dir" -B "$soak_build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG' -DSHM_IPC_SANITIZE=
cmake --build "$soak_build_dir" --target bench_acceptance -j "${SOAK_BUILD_JOBS:-4}"
mkdir -p -- "$(dirname -- "$soak_output_dir")"

nohup python3 -u "$soak_repo_dir/bench/run_soak.py" \
    "$soak_build_dir/bench_acceptance" "$soak_output_dir" \
    --seconds 604800 "$@" > "$soak_console_log" 2>&1 < /dev/null &
soak_runner_pid=$!

# Confirm both native processes were launched, or expose startup failure.
python3 - "$soak_output_dir" "$soak_runner_pid" <<'PY'
import json
from pathlib import Path
import sys
import time
out=Path(sys.argv[1]); runner=int(sys.argv[2]); deadline=time.monotonic()+12
while time.monotonic()<deadline:
    status_file=out/'status.json'; pids_file=out/'pids.json'
    if status_file.exists():
        status=json.loads(status_file.read_text())
        if status['state'] in ['failed','interrupted']:
            raise SystemExit('Startup failed: '+str(status.get('reason')))
        if status['state']=='passed':
            print('Short test already completed successfully.')
            break
        if pids_file.exists():
            pids=json.loads(pids_file.read_text())
            if all(key in pids for key in ['server_pid','client_pid']):
                if all(Path('/proc',str(pids[key])).exists() for key in ['server_pid','client_pid']):
                    print('Supervisor and both native processes launched.')
                    break
    if not Path('/proc',str(runner)).exists():
        raise SystemExit('Supervisor exited; inspect '+str(out)+'.console.log')
    time.sleep(.1)
else:
    raise SystemExit('Startup is not yet confirmed; inspect '+str(out)+'.console.log')
PY

printf '\nSupervisor PID: %s\nOutput: %s\nConsole log: %s\n' \
    "$soak_runner_pid" "$soak_output_dir" "$soak_console_log"
printf 'Status: python3 %q --status %q\n' "$soak_repo_dir/bench/run_soak.py" "$soak_output_dir"
printf 'Follow: tail -f %q\n' "$soak_console_log"
printf 'Stop: python3 %q --stop %q\n' "$soak_repo_dir/bench/run_soak.py" "$soak_output_dir"
