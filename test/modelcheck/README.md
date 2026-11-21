# RingBuffer finite RC11 model checks

Run with GenMC v0.10.0 built against LLVM 16:

```sh
python3 test/modelcheck/run.py /tmp/shm-genmc/genmc /tmp/shm-ring-rc11-results
```

The harness requires ten positive checks and fifteen negative controls to
match expectations. Negative controls must produce a safety assertion or a
non-atomic race; compilation errors, unsupported intrinsics and timeouts do
not count as detection. Production headers are only copied to a temporary
folder for mutations. GenMC race detection stays enabled.

See [the validation report](../../docs/ringbuf_rc11_validation_2026-11-21.md)
for model-to-code correspondence, scope, tool caveats and installation.
These files are GenMC inputs, not ordinary CMake test executables:
`__VERIFIER_assume` represents a successful read/write or completed wait path;
it is not a production API.

Additional assumption audit and independent SMT checks:

```sh
python3 test/modelcheck/run_audit.py /tmp/shm-genmc/genmc /tmp/shm-rearm-audit
# In a venv with z3-solver==4.12.2.0:
python3 test/modelcheck/wait_sc_constraints.py /tmp/shm-wait-smt
python3 test/modelcheck/ring_invariant_smt.py /tmp/shm-invariant-smt
```

See [the proof audit](../../docs/ringbuf_proof_audit_2026-11-21.md).
The SMT encodings prove the stated reduced constraints and arithmetic
obligations; they are not a full independent C++ memory model implementation.

Protocol state closure (Python standard library):

```sh
python3 test/modelcheck/wait_protocol_graph.py /tmp/shm-protocol-closure
```

This explores all reachable states of finite SC control/event abstractions,
including pending notification obligations and cursor ABA negative controls.
See [scope and progress assumptions](../../docs/ringbuf_protocol_closure_2026-11-21.md).
