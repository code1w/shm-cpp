#!/usr/bin/env python3
"""Plot complete RPC metrics without treating loopback as a cross-host NIC test."""
import json
from pathlib import Path
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

out=Path(sys.argv[1]);data=json.loads((out/'summary.json').read_text())
labels=[]
for row in data:
    size=str(row['bytes'])+'B' if row['bytes']<1024 else str(row['bytes']//1024)+'KiB'
    labels.append(f"{size}\nbatch {row['batch_min']}–{row['batch_max']}")
positions=np.arange(len(data));modes=['uds','shm-eventfd','shm-busy'];colors=['#64748b','#2563eb','#16a34a']
fig,axes=plt.subplots(3,1,figsize=(max(13,len(data)*0.65),10),sharex=True,constrained_layout=True)
flow_key = 'duplex_protobuf_mib_s' if all(row.get('codec') == 'protobuf' for row in data) else 'duplex_payload_mib_s'
for index,(key,title,unit) in enumerate([('rpc_s','Completed RPC throughput','RPC/s'),('p50_us','End-to-end RTT P50','microseconds'),(flow_key,'TCP request + reply message bytes','MiB/s')]):
    for mode_index,(mode,color) in enumerate(zip(modes,colors)):
        values=[row.get(mode,{}).get(key,float('nan')) for row in data]
        axes[index].bar(positions+(mode_index-1)*.25,values,.25,label=mode,color=color)
    axes[index].set_yscale('log');axes[index].set_ylabel(unit);axes[index].set_title(title);axes[index].grid(axis='y',alpha=.2);axes[index].legend()
axes[-1].set_xticks(positions,labels)
loopback=all(row['topology']==['single-host-tcp-loopback'] for row in data)
fig.suptitle('Four-process chain: local IPC / TCP / local IPC\n'+('Single-host TCP loopback verification; not a physical cross-host performance result' if loopback else 'User-managed topology; verify the deployed network separately'))
fig.savefig(out/'chain_metrics.png',dpi=150)
