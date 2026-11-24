#!/usr/bin/env python3
"""Standalone random-batch size curves; shaded ranges are observations, not CIs."""
import json
from pathlib import Path
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

out = Path(sys.argv[1])
data = json.loads((out / 'summary.json').read_text())
fig, axes = plt.subplots(2, 2, figsize=(14, 9), constrained_layout=True)
colors = {'shm-eventfd': '#2563eb', 'shm-busy': '#16a34a', 'socket': '#ea580c'}
for row, profile in enumerate(['small', 'burst']):
    points = sorted((x for x in data if x['profile'] == profile), key=lambda x: x['bytes'])
    sizes = [x['bytes'] for x in points]
    for mode, color in colors.items():
        axes[row, 0].plot(sizes, [x[mode]['rpc_s'] for x in points], marker='o', ms=3, label=mode, color=color)
        axes[row, 0].fill_between(sizes, [x[mode]['rpc_range'][0] for x in points],
                               [x[mode]['rpc_range'][1] for x in points], color=color, alpha=.12)
        axes[row, 1].plot(sizes, [x[mode]['p50_us'] for x in points], marker='o', ms=3, label=mode, color=color)
        axes[row, 1].plot(sizes, [x[mode]['p999_us'] for x in points], linestyle='--', color=color, alpha=.65)
    axes[row, 0].set_title(profile + ': completed RPC/s (observed three-run ranges)')
    axes[row, 1].set_title(profile + ': RTT P50 solid / P99.9 dashed')
    axes[row, 0].set_ylabel('RPC/s'); axes[row, 1].set_ylabel('microseconds')
    for ax in axes[row]:
        ax.set_xscale('log', base=2); ax.set_yscale('log')
        ax.set_xlabel('Message body bytes'); ax.grid(True, which='both', alpha=.2); ax.legend()
fig.suptitle('Variable batch: SHM/eventfd vs SHM/busy vs Unix socket\nUniform batches; ranges reduce for large messages; busy uses dedicated CPU cores')
fig.savefig(out / 'size_curves.png', dpi=160)
