#!/usr/bin/env python3
"""Explicit SC protocol graph, all reachable states until closure.
SC scheduling is NOT an RC11 fence model. It checks control/event obligations.
A reduced cursor modulus exposes ABA under an unbounded single drain call.
Usage: python wait_protocol_graph.py OUTPUT
"""
from collections import deque
from dataclasses import dataclass,asdict,replace
import hashlib
import json
from pathlib import Path
import sys

@dataclass(frozen=True)
class State:
    used:int=0
    pos:int=0
    flag:int=2
    event:bool=False
    writer:int=0
    loaded:int=-1
    reader:int=0
    seen:int=0
    progress:int=0

# Writer: 0 publish, 1 fence, 2 flag load, 3 exchange, 4 eventfd write.
# Reader: 0 drain eventfd, 1 capture baseline, 2 peek, 3 commit frame,
#         4 set Waiting, 5 fence, 6 recheck, 7 cancel, 8 poll.
def edges(s,capacity,modulus,frame,bounded,drop_notify=False):
    if s.writer==0 and s.used<capacity:
        active=s.reader in range(2,8)
        if not (bounded and active and s.progress>=modulus-1):
            yield 'W: publish byte',replace(s,used=s.used+1,pos=(s.pos+1)%modulus,writer=1,
                progress=min(modulus,s.progress+1) if active else 0)
    elif s.writer==1: yield 'W: fence',replace(s,writer=2)
    elif s.writer==2:
        if s.flag==0: yield 'W: observes Awake',replace(s,writer=0)
        elif s.flag==2: yield 'W: observes Always',replace(s,writer=4)
        else: yield 'W: observes Waiting',replace(s,writer=3,loaded=1)
    elif s.writer==3:
        yield 'W: exchange Waiting to Awake',replace(s,flag=0,writer=4 if s.flag!=0 else 0,loaded=-1)
    elif s.writer==4:
        yield 'W: fulfill notification' if not drop_notify else 'W: DROP notification',replace(s,event=s.event if drop_notify else True,writer=0)
    if s.reader==0: yield 'R: drain notification',replace(s,event=False,reader=1)
    elif s.reader==1: yield 'R: capture write baseline',replace(s,seen=s.pos,progress=0,reader=2)
    elif s.reader==2: yield 'R: peek',replace(s,reader=3 if s.used>=frame else 4)
    elif s.reader==3: yield 'R: consume complete frame',replace(s,used=s.used-frame,reader=2)
    elif s.reader==4: yield 'R: set Waiting',replace(s,flag=1,reader=5)
    elif s.reader==5: yield 'R: fence',replace(s,reader=6)
    elif s.reader==6:
        yield 'R: recheck baseline',replace(s,reader=8 if s.pos==s.seen else 7,progress=0 if s.pos==s.seen else s.progress)
    elif s.reader==7: yield 'R: cancel Waiting',replace(s,flag=0,reader=1)
    elif s.reader==8 and s.event: yield 'R: poll sees event',replace(s,reader=0,seen=0,progress=0)

def run(capacity,modulus,frame,bounded,drop_notify=False):
    initial=State(); queue=deque([initial]); predecessor={initial:None}; transitions=0
    bad=None; in_flight=0
    while queue:
        s=queue.popleft()
        assert 0<=s.used<=capacity
        if s.reader==8 and s.used>=frame and not s.event:
            if s.writer==0: bad=s;break
            # Unsent notification is an obligation, not yet a lost wake.
            in_flight+=1
        for action,nxt in edges(s,capacity,modulus,frame,bounded,drop_notify):
            transitions+=1
            if nxt not in predecessor:
                predecessor[nxt]=(s,action);queue.append(nxt)
    trace=[]
    if bad:
        cursor=bad
        while predecessor[cursor]:
            prev,action=predecessor[cursor]
            trace.append({'action':action,'before':asdict(prev),'after':asdict(cursor)})
            cursor=prev
        trace.reverse()
    progress_check=None
    if bad is None:
        # Complete-frame availability is monotone until a consume edge.
        # If the induced graph without consumption is acyclic, no infinite
        # sequence of actual protocol steps can leave a complete frame pending.
        pending={v for v in predecessor if v.used>=frame}
        adjacency={v:[] for v in pending}; indegree={v:0 for v in pending}
        terminal=0
        for v in pending:
            outgoing=list(edges(v,capacity,modulus,frame,bounded,drop_notify))
            if not outgoing: terminal+=1
            for action,nxt in outgoing:
                if action!='R: consume complete frame' and nxt in pending:
                    adjacency[v].append(nxt);indegree[nxt]+=1
        zero=deque(v for v in pending if indegree[v]==0);removed=0
        while zero:
            v=zero.popleft();removed+=1
            for nxt in adjacency[v]:
                indegree[nxt]-=1
                if indegree[nxt]==0:zero.append(nxt)
        progress_check={'pending_states':len(pending),'no_consume_cycle_states':len(pending)-removed,
            'pending_terminal_states':terminal}
        assert terminal==0 and removed==len(pending),progress_check
    return {'capacity':capacity,'modulus':modulus,'frame_bytes':frame,'bounded_drain_progress':bounded,
        'drop_notify':drop_notify,'closed':bad is None,'visited_states':len(predecessor),
        'transitions':transitions,'progress_check':progress_check,'in_flight_sleep_states':in_flight,'bad_state':asdict(bad) if bad else None,'trace':trace}

out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
results=[]
for capacity,modulus,frame in [(2,8,1),(2,8,2),(4,8,2)]:
    result=run(capacity,modulus,frame,True)
    assert result['closed'],result
    results.append(result)
for name,bounded,drop in [('unbounded_aba',False,False),('dropped_notification',True,True)]:
    result=run(2,8,2,bounded,drop)
    assert not result['closed'],name
    (out/(name+'.json')).write_text(json.dumps(result,indent=2)+'\n')
    results.append(result)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
(out/'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest()+'  test/modelcheck/wait_protocol_graph.py\n')
for r in results:print('C',r['capacity'],'M',r['modulus'],'frame',r['frame_bytes'],'bounded',r['bounded_drain_progress'],'drop',r['drop_notify'],'closed',r['closed'],'states',r['visited_states'])
