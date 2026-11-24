#!/usr/bin/env python3
"""Standalone size-curve artifact; batch sizes are explicitly annotated."""
import json
from pathlib import Path
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
out=Path(sys.argv[1]);data=json.loads((out/'summary.json').read_text())
serial=all(x['batch']==1 for x in data)
rows=[]
for size in sorted({x['bytes'] for x in data}):
 candidates=[x for x in data if x['bytes']==size]
 wanted=1 if serial else 64 if size==16 else 4 if size==1048576 else max(x['batch'] for x in candidates)
 row=next(x for x in candidates if x['batch']==wanted)
 rows.append(row)
size=[x['bytes'] for x in rows]
labels=[str(n)+'B' if n<1024 else str(n//1024)+'KiB' if n<1048576 else str(n//1048576)+'MiB' for n in size]
fig,axes=plt.subplots(2,1,figsize=(13,8),sharex=True)
colors={'shm':'#176b9c','socket':'#d16a2d'}
for side in ['shm','socket']:
 axes[0].plot(size,[x[side]['payload_mib_s']/1024 for x in rows],'-o',label=side.upper(),color=colors[side])
 axes[0].fill_between(size,[x[side]['rpc_s_range'][0]*x['bytes']/1073741824 for x in rows],[x[side]['rpc_s_range'][1]*x['bytes']/1073741824 for x in rows],color=colors[side],alpha=.12)
 axes[1].plot(size,[x[side]['p50_us'] for x in rows],'-o',label=side.upper()+' P50',color=colors[side])
 axes[1].plot(size,[x[side]['p999_us'] for x in rows],'--s',label=side.upper()+' P99.9',color=colors[side],alpha=.7)
for x in rows:
 axes[0].annotate('b'+str(x['batch']),(x['bytes'],x['shm']['payload_mib_s']/1024),xytext=(0,8),textcoords='offset points',ha='center',fontsize=8)
for ax in axes:
 ax.set_xscale('log',base=2);ax.grid(True,alpha=.2);ax.legend()
axes[0].set_ylabel('One-way POD payload GiB/s')
axes[1].set_ylabel('Per-request RTT (microseconds)');axes[1].set_yscale('log')
axes[1].set_xticks(size);axes[1].set_xticklabels(labels,rotation=45,ha='right');axes[1].set_xlabel('POD size (wire includes additional 4-byte tag)')
mode='All batch1 (one outstanding request)' if serial else 'Mixed batches (b64 up to 32KiB, b32/b16/b4 beyond)'
fig.suptitle('SHM/eventfd vs Unix stream socket | common codec/EventLoop | paired medians (band: observed 3-run range)\n'+mode+'; CPU1/3 NUMA1')
fig.tight_layout();fig.savefig(out/'size_curves.png',dpi=160);plt.close(fig)
print(out/'size_curves.png')
