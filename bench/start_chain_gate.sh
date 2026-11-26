#!/usr/bin/env bash
set -euo pipefail
chain_script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 "$chain_script_dir/run_chain_host.py" gate "$@"
