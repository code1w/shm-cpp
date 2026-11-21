#!/usr/bin/env python3
"""Targeted correction of prior rearm-model assumptions; no production edits."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[2]).resolve(); out.mkdir(parents=True,exist_ok=True)
checker=str(Path(sys.argv[1]).resolve())
version=subprocess.check_output([checker,'--version'],text=True)
assert 'v0.10.0' in version and 'LLVM 16.' in version
cases=[('raw_bytes',['-DFRAME_BYTES=1'],False),
       ('partial_frame',[],False),
       ('incomplete_only',['-DPUBLICATIONS=1'],False),
       ('no_reader_fence',['-DREADER_FENCE=0'],True),
       ('no_writer_fence',['-DWRITER_FENCE=0'],True)]
results=[]
for name,flags,negative in cases:
    command=[checker,'--rc11','--disable-estimation','--disable-sr','--disable-ipr','--print-error-trace','--','-std=c11']+flags+[str(root/'test/modelcheck/reader_rearm_audit.c')]
    run=subprocess.run(command,text=True,capture_output=True,timeout=60)
    text=run.stdout+run.stderr
    (out/(name+'.log')).write_text(text)
    matched=(run.returncode!=0 and 'Error: Safety violation!' in text) if negative else (run.returncode==0 and 'Verification complete. No errors were detected.' in text and 'Number of blocked executions seen:' not in text)
    results.append({'name':name,'expected':'counterexample' if negative else 'pass_no_assume','matched':matched,'returncode':run.returncode,'command':command})
    print(name,matched)
    if not matched: raise RuntimeError(name)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
(out/'checker-version.txt').write_text(version)
(out/'sources.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(root))+'\n' for p in [root/'test/modelcheck/reader_rearm_audit.c',root/'test/modelcheck/run_audit.py',root/'include/shm_ipc/shm_connection.hpp',root/'include/shm_ipc/ringbuf.hpp']))
