#!/usr/bin/env python3
"""Check that the oracle rejects representative bugs; never edits production.
Usage: python3 test/ringbuf_mutation_check.py [output-directory]
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="ring-mutations-"))
output.mkdir(parents=True, exist_ok=True)
source = (root / "include/shm_ipc/ringbuf.hpp").read_text()
mutations = {
    "publish_before_flush": ("        w_ += len;", "        w_ += len;\n        hdr_->write_pos.store(w_, std::memory_order_release);"),
    "cancel_publishes": ("        w_            = w_start_;", "        hdr_->write_pos.store(w_, std::memory_order_release);\n        w_            = w_start_;"),
    "reject_numeric_wrap": ("        if (w_ - r_ > Capacity || len > Capacity - (w_ - r_))", "        if (w_ + len < w_ || w_ - r_ > Capacity || len > Capacity - (w_ - r_))"),
    "accept_mismatched_reservation": ("if (!hdr_ || !reserved_ || len != reserved_len_)", "if (!hdr_ || !reserved_)"),
}
results = []
for name, (before, after) in mutations.items():
    assert before in source, name
    variant = output / name
    (variant / "shm_ipc").mkdir(parents=True, exist_ok=True)
    (variant / "shm_ipc/ringbuf.hpp").write_text(source.replace(before, after, 1))
    binary = variant / "test_model"
    subprocess.run(["c++", "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-pedantic",
                    "-I" + str(variant), "-I" + str(root / "include"),
                    str(root / "test/test_ringbuf_model.cpp"), "-pthread", "-o", str(binary)], check=True)
    # Disable cores while retaining a real nonzero/assert failure as evidence.
    import resource
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, preexec_fn=no_core)
    (variant / "run.log").write_text(run.stdout + run.stderr)
    detected = run.returncode != 0 and "FAIL" in run.stderr
    results.append({"mutation": name, "returncode": run.returncode, "detected": detected})
    if not detected:
        raise RuntimeError("oracle failed to detect " + name)
(output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
print(json.dumps(results, indent=2))
