#!/usr/bin/env python3
"""Unbounded logical-position arithmetic, not a C++ memory-model proof.
Prove preservation for every state satisfying the stated preconditions.
Usage: python ring_invariant_smt.py OUTPUT (z3-solver 4.12.2.0).
"""
import json
from pathlib import Path
import sys
import z3
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
assert z3.get_version_string()=='4.12.2'
R,W,L,S,C,N,P=z3.Ints('read published local snapshot capacity amount prefix')
pre=z3.And(C>0,0<=R,R<=W,W<=L,S>=0,S<=R,L-S<=C)
cases={
    'pending_write': (z3.And(N>0,N<=C-(L-S)), z3.And(R<=W,W<=L+N,L+N-S<=C,L+N-R<=C)),
    'publish': (z3.BoolVal(True), z3.And(R<=L,L-R<=C)),
    'consume': (z3.And(N>=0,N<=W-R),z3.And(S<=R+N,R+N<=W,W<=L,L-S<=C,L-(R+N)<=C)),
    'cancel': (z3.BoolVal(True),z3.And(R<=W,W-S<=C)),
    'rewind': (z3.And(P>=0,P<=L-W),z3.And(R<=W,W<=W+P,W+P-S<=C)),
    'fresh_snapshot': (z3.And(N>=S,N<=R),z3.And(N<=R,R<=W,W<=L,L-N<=C)),
    'ordinary_write': (z3.And(L==W,N>0,N<=C-(W-S)),z3.And(R<=W+N,W+N-R<=C)),
}
base=z3.Solver();base.add(C>0,R==0,W==0,L==0,S==0,z3.Not(pre))
assert base.check()==z3.unsat
results=[]
for name,(guard,post) in cases.items():
    s=z3.Solver();s.add(pre,guard,z3.Not(post))
    assert s.check()==z3.unsat,name
    (out/(name+'.smt2')).write_text(s.to_smt2())
    # Every transition must have a realizable pre-state/guard.
    enabled=z3.Solver();enabled.add(pre,guard)
    nontrivial={'pending_write':N>0,'publish':L>W,'consume':N>0,
        'cancel':L>W,'rewind':z3.And(P>0,P<L-W),
        'fresh_snapshot':N>S,'ordinary_write':N>0}[name]
    enabled.add(nontrivial);assert enabled.check()==z3.sat
    results.append({'transition':name,'counterexample':'unsat','enabled':'sat'})
# Negative control: checking only amount <= capacity ignores current
# occupancy and must allow a global-capacity counterexample.
s=z3.Solver();s.add(pre,L==W,N>0,N<=C,z3.Not(W+N-R<=C))
assert s.check()==z3.sat
(out/'unsafe_guard_witness.txt').write_text(str(s.model())+'\n')
(out/'results.json').write_text(json.dumps({'solver':z3.get_version_string(),
    'scope':'logical unbounded positions; memory visibility is a separate obligation',
    'transitions':results,'unsafe_guard_detected':True},indent=2)+'\n')
print('PASS: seven unbounded arithmetic preservation obligations, non-vacuity and unsafe-guard counterexample')
