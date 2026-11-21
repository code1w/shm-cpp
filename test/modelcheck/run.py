#!/usr/bin/env python3
"""Finite RC11 checks and negative controls. Usage: run.py /path/to/genmc OUT.
No production edits. A negative case must produce a safety/race counterexample,
not a compiler error, timeout, unsupported intrinsic, or optimization warning.
"""
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
checker = str(Path(sys.argv[1]).resolve())
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
version = subprocess.check_output([checker, "--version"], text=True)
(out / "checker-version.txt").write_text(version)
if "v0.10.0" not in version or "LLVM 16." not in version:
    raise RuntimeError("This harness was validated against GenMC v0.10.0 / LLVM 16; review other versions first")
options = ["--rc11", "--disable-estimation", "--disable-sr", "--disable-ipr", "--print-error-trace"]
cpp = ["-O1", "-x", "c++", "-std=c++17", "-D__cpp_lib_atomic_is_always_lock_free=201603L"]
cases = []
def add(name, source, defines=(), negative=False, mutation=None):
    cases.append((name, source, list(defines), negative, mutation))
for mode in range(3):
    add("wait_mode_"+str(mode), "reader_wait.c", ["-DINITIAL_MODE="+str(mode)])
for source in ["reader_rearm.c", "publication.c", "reuse.c", "actual_ring_wait.cpp", "actual_ring_publication.cpp", "actual_ring_reuse.cpp", "actual_ring_batch.cpp"]:
    add(Path(source).stem,source)
for name,defines in [("no_reader_fence",["-DREADER_FENCE=0"]), ("no_writer_fence",["-DWRITER_FENCE=0"]), ("acq_rel_fences",["-DFENCE_ORDER=memory_order_acq_rel"])]:
    add(name,"reader_wait.c",defines,True)
for name,source,define in [
    ("publish_relaxed","publication.c","PUBLISH_ORDER"),
    ("observe_relaxed","publication.c","OBSERVE_ORDER"),
    ("reclaim_relaxed","reuse.c","RECLAIM_ORDER"),
    ("space_relaxed","reuse.c","SPACE_ORDER")]:
    add(name,source,["-D"+define+"=memory_order_relaxed"],True)
for side in ["READER","WRITER"]:
    add("rearm_no_"+side.lower()+"_fence","reader_rearm.c",["-D"+side+"_FENCE=0"],True)
add("actual_wait_no_reader_fence","actual_ring_wait.cpp",negative=True,mutation="reader_fence")
add("actual_wait_no_writer_fence","actual_ring_wait.cpp",negative=True,mutation="writer_fence")
add("actual_publish_relaxed","actual_ring_publication.cpp",negative=True,mutation="publication")
add("actual_reclaim_relaxed","actual_ring_reuse.cpp",negative=True,mutation="reclamation")
add("actual_space_relaxed","actual_ring_reuse.cpp",negative=True,mutation="space")
add("actual_batch_publish_relaxed","actual_ring_batch.cpp",negative=True,mutation="batch_publication")
header=(root/"include/shm_ipc/ringbuf.hpp").read_text()
results=[]
with tempfile.TemporaryDirectory(prefix="shm-rc11-") as temp:
    for name,source,defines,negative,mutation in cases:
        flags=cpp.copy() if source.endswith(".cpp") else ["-std=c11"]
        if mutation:
            variant=Path(temp)/name/"shm_ipc"
            variant.mkdir(parents=True)
            if mutation in ["reader_fence","writer_fence"]:
                needle="std::atomic_thread_fence(std::memory_order_seq_cst);"
                assert header.count(needle)==2
                index=header.find(needle) if mutation=="reader_fence" else header.rfind(needle)
                modified=header[:index]+"/* negative control: removed fence */"+header[index+len(needle):]
            else:
                before={"publication":"hdr->write_pos.store(w + len, std::memory_order_release);",
                        "batch_publication":"hdr_->write_pos.store(w_, std::memory_order_release);",
                        "reclamation":"hdr->read_pos.store(r + len, std::memory_order_release);",
                        "space":"hdr->read_pos.load(std::memory_order_acquire)"}[mutation]
                assert before in header
                after=before.replace("memory_order_release","memory_order_relaxed").replace("memory_order_acquire","memory_order_relaxed")
                modified=header.replace(before,after)
            (variant/"ringbuf.hpp").write_text(modified)
            patch=difflib.unified_diff(header.splitlines(True),modified.splitlines(True),
                fromfile="production/ringbuf.hpp",tofile=name+"/ringbuf.hpp")
            (out/(name+".header.patch")).write_text("".join(patch))
            flags.append("-I"+str(variant.parent))
        flags.extend(["-I"+str(root/"include")]+defines)
        command=[checker]+options+["--"]+flags+[str(root/"test/modelcheck"/source)]
        run=subprocess.run(command,capture_output=True,text=True,timeout=60)
        output=run.stdout+run.stderr
        (out/(name+".log")).write_text(output)
        counterexample="Error: Safety violation!" in output or "Error: Non-atomic race!" in output
        complete="Verification complete. No errors were detected." in output
        matches=re.search(r"Number of complete executions explored: (\d+)",output)
        count=int(matches[1]) if matches else None
        expected=(run.returncode!=0 and counterexample) if negative else (run.returncode==0 and complete and count is not None and count>0)
        result={"name":name,"expected":"counterexample" if negative else "pass","returncode":run.returncode,"complete_executions":count,"atomic_write_warning":"Warning: Write-write race!" in output,"matched":expected,"command":command}
        results.append(result)
        print(name, "OK" if expected else "UNEXPECTED", "executions", count, flush=True)
        if not expected:
            (out/"results.json").write_text(json.dumps(results,indent=2)+"\n")
            raise RuntimeError("Unexpected model result; inspect "+str(out/(name+".log")))
(out/"results.json").write_text(json.dumps(results,indent=2)+"\n")
paths=sorted((root/"test/modelcheck").glob("*.c"))+sorted((root/"test/modelcheck").glob("*.cpp"))+[root/"test/modelcheck/run.py",root/"include/shm_ipc/ringbuf.hpp"]
(out/"sources.sha256").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest()+"  "+str(p.relative_to(root))+"\n" for p in paths))
print("All",len(results),"checks matched expectations")
