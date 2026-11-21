#!/usr/bin/env python3
"""Independent Z3 cross-check of the two-miss wait/notify litmus.
This is a reduced SC-fence coherence encoding, NOT a complete C++/RC11 checker.
Reference: https://eel.is/c++draft/atomics.order#3 and #4.4
Usage: python wait_sc_constraints.py OUTPUT (requires z3-solver 4.12.2.0).
"""
import itertools
import json
from pathlib import Path
import sys
import z3

out=Path(sys.argv[1]); out.mkdir(parents=True,exist_ok=True)
if z3.get_version_string()!='4.12.2':
    raise RuntimeError('Review other solver versions before accepting evidence')

def encode(reader_sc,writer_sc,omit_edge=None):
    s=z3.Solver()
    saw_publish,saw_wait=z3.Bools('reader_saw_publication writer_saw_waiting')
    rf,wf=z3.Ints('reader_fence_rank writer_fence_rank')
    if reader_sc and writer_sc:
        s.assert_and_track(z3.And(rf>=0,rf<2,wf>=0,wf<2,rf!=wf),'strict_total_sc_order')
        # Read initial write_pos -> fr -> new publication; reader fence hb read,
        # publication hb writer fence. atomics.order 4.4 induces RF < WF.
        if omit_edge!='write_pos':
            s.assert_and_track(z3.Implies(z3.Not(saw_publish),rf<wf),'write_pos_fr_fence_order')
        # Read initial Awake -> fr -> Waiting; writer fence hb read,
        # Waiting hb reader fence. Same axiom induces WF < RF.
        if omit_edge!='reader_wait':
            s.assert_and_track(z3.Implies(z3.Not(saw_wait),wf<rf),'reader_wait_fr_fence_order')
    # With fewer than two SC fences, the opposed SC-order constraints cannot
    # be formed. On the both-initial-reads branch release/acquire creates no
    # cross-thread synchronizes-with edge, so both misses remain permitted.
    return s,saw_publish,saw_wait

results=[]
for name,reader,writer,expected in [('both_sc',True,True,'unsat'),
    ('no_reader_sc',False,True,'sat'),('no_writer_sc',True,False,'sat'),
    ('neither_sc',False,False,'sat')]:
    solver,pub,wait=encode(reader,writer)
    solver.assert_and_track(z3.And(z3.Not(pub),z3.Not(wait)),'lost_wakeup_candidate')
    result=solver.check()
    assert str(result)==expected,(name,result)
    # assert_and_track uses implicit assumptions inside Solver.check().
    # SMT2 export must assert their labels too, otherwise a replay could
    # disable every axiom by assigning the labels false.
    labels=['lost_wakeup_candidate']
    if reader and writer:
        labels+=['strict_total_sc_order','write_pos_fr_fence_order','reader_wait_fr_fence_order']
    smt=solver.to_smt2().replace('(check-sat)',
        '\n'.join('(assert '+label+')' for label in labels)+'\n(check-sat)')
    replay=z3.Solver(); replay.add(z3.parse_smt2_string(smt))
    assert replay.check()==result
    (out/(name+'.smt2')).write_text(smt)
    core=[str(x) for x in solver.unsat_core()] if result==z3.unsat else []
    witness=str(solver.model()) if result==z3.sat else None
    results.append({'name':name,'bad_state':str(result),'unsat_core':core,'witness':witness})
# Non-vacuity: all other read-source choices must have a legal fence order.
for pub,wait in itertools.product([False,True],repeat=2):
    s,p,w=encode(True,True); s.add(p==pub,w==wait)
    assert s.check()==(z3.unsat if not pub and not wait else z3.sat)
# Encoder sensitivity: removing either coherence-derived edge admits bad state.
for edge in ['write_pos','reader_wait']:
    s,p,w=encode(True,True,edge); s.add(z3.Not(p),z3.Not(w))
    assert s.check()==z3.sat
(out/'results.json').write_text(json.dumps({'solver':z3.get_version_string(),
    'scope':'two-miss branch, not full C++ memory model','cases':results,
    'non_vacuity_read_source_cases':4,'encoder_edge_mutations_detected':2},indent=2)+'\n')
print('PASS: paired SC fences exclude both misses; weakened cases admit it; non-vacuity and two edge mutations checked')
