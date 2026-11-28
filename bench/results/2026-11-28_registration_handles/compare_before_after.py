import sys,json,hashlib,statistics
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,'bench')
import compare_random_batch as driver
out=Path('bench/results/2026-11-28_registration_handles/performance');out.mkdir(parents=True,exist_ok=True)
binaries={'before':'/tmp/shm-registration-before/bench_acceptance','after':'/tmp/shm-registration-after/bench_acceptance'}
cases=[(16,1),(16,64),(16,256),(4096,64),(1048576,2)]
args=SimpleNamespace(server_cpu=1,client_cpu=3,seconds=5,smoke=False,binary='')
rows=[]
for repeat in range(3):
    sequence=list(enumerate(cases))
    if repeat%2: sequence.reverse()
    for index,(size,batch) in sequence:
        for mode_index,mode in enumerate(['shm-eventfd','shm-busy']):
            versions=['before','after'] if (repeat+index+mode_index)%2==0 else ['after','before']
            seed=0x4321+index*4099+repeat*65537
            group=[]
            for pos,version in enumerate(versions):
                args.binary=binaries[version];dest=out/version;dest.mkdir(exist_ok=True)
                row=driver.run_case(args,dest,size,batch,batch,f'fixed{batch}',mode,repeat+1,pos+1,seed)
                if not row:
                    (out/'status.json').write_text(json.dumps(dict(state='failed',completed=len(rows),total=60)))
                    sys.exit(1)
                row.update(version=version,bytes=size,batch=batch)
                rows.append(row);group.append(row)
                (out/'samples.json').write_text(json.dumps(rows,indent=2)+'\n')
                (out/'status.json').write_text(json.dumps(dict(state='running',completed=len(rows),total=60))+'\n')
            if len({x['batch_stats']['prefix_hash'] for x in group})!=1:sys.exit(2)
summary=[]
for size,batch in cases:
    for mode in ['shm-eventfd','shm-busy']:
        samples=[x for x in rows if x['bytes']==size and x['batch']==batch and x['mode']==mode]
        result=dict(bytes=size,batch=batch,mode=mode)
        gains=[]
        for repeat in [1,2,3]:
            pair={x['version']:x for x in samples if x['round']==repeat}
            gains.append((pair['after']['msg_s']/pair['before']['msg_s']-1)*100)
        result['paired_rpc_gain_pct']=statistics.median(gains)
        result['paired_rpc_gain_range_pct']=[min(gains),max(gains)]
        for version in binaries:
            selected=[x for x in samples if x['version']==version]
            result[version]=dict(rpc_s=statistics.median(x['msg_s'] for x in selected),
                rpc_range=[min(x['msg_s'] for x in selected),max(x['msg_s'] for x in selected)],
                p50_us=statistics.median(x['p50_ns']/1000 for x in selected))
        summary.append(result)
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(out/'status.json').write_text(json.dumps(dict(state='passed',completed=len(rows),total=60))+'\n')
