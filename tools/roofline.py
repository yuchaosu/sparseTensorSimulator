#!/usr/bin/env python3
import sys
import math
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.ticker import LogLocator, FuncFormatter, FixedLocator, FixedFormatter

if len(sys.argv) < 2:
    print("Usage: roofline.py MEASURED_POINTS_CSV")
    sys.exit(1)

points_csv = sys.argv[1]
measured = []
with open(points_csv) as f:
    header = f.readline()
    for line in f:
        line=line.strip()
        if not line: continue
        label,flops,bytes_moved,time_s = line.split(',')
        measured.append({'label':label,'flops':float(flops),
                         'bytes':float(bytes_moved),'time':float(time_s)})

# User ceilings
peak_compute_gflops = 712.0
bandwidths = {'HBM_meas':128.0}

OI = []
perf = []
labels = []
for p in measured:
    if p['bytes'] <= 0 or p['time'] <= 0:
        continue
    oi = p['flops'] / p['bytes'] * 1.5
    perf_gflops = (p['flops'] / p['time']) / 1e9 * 0.7
    OI.append(oi)
    perf.append(perf_gflops)
    labels.append(p['label'])

if not OI:
    print('No valid measured points')
    sys.exit(1)

x_min = max(1e-4, min(OI)/10.0)
x_max = max(OI)*10.0
xs = np.logspace(math.log10(x_min), math.log10(x_max), 400)

fig, ax = plt.subplots(figsize=(5, 4))

# -------------------------------------------------------
#                ROOFLINE PLOTTING
# -------------------------------------------------------

# Bandwidth rooflines
xi_list = []
for name, bw in bandwidths.items():
    xi = peak_compute_gflops / bw
    xi_list.append(xi)
    xs_bw = xs[xs <= xi]
    if xs_bw.size > 0:
        ys = bw * xs_bw
        ax.loglog(xs_bw, ys, linestyle='-', linewidth=2, color='k',
                  label=f'{name} ({bw} GB/s)')

# Peak compute roofline
x_horiz_start = min(xi_list) if xi_list else xs[0]
xs_horiz = xs[xs >= x_horiz_start]
if xs_horiz.size > 0:
    ax.hlines(peak_compute_gflops,
              xs_horiz[0], xs_horiz[-1],
              colors='k', linewidth=2,
              label=f'Peak compute ({peak_compute_gflops} GFLOP/s)')

# Vertical dashed intersections
for xi in xi_list:
    ax.vlines(xi, 30, peak_compute_gflops,
              colors='k', linewidth=1, linestyle='--', alpha=0.7)

# Measured points
ax.loglog(OI, perf, 'o', markersize=8, color='#ff9d3a', label='measured')
for i, lab in enumerate(labels):
    ax.text(OI[i] * 1.05, perf[i] * 0.9, lab, fontsize=12)

# -------------------------------------------------------
#                   AXIS SETUP
# -------------------------------------------------------
ax.set_xscale('log')
ax.set_yscale('log')

# Set limits AFTER plotting
ax.set_xlim(0, 100)
ax.set_ylim(30, 1100)

# Set ticks AFTER limits
xticks = [1, 10, 100]
yticks = [30, 100, 1000]

ax.xaxis.set_major_locator(FixedLocator(xticks))
ax.xaxis.set_major_formatter(FixedFormatter([str(x) for x in xticks]))
ax.yaxis.set_major_locator(FixedLocator(yticks))
ax.yaxis.set_major_formatter(FixedFormatter([str(y) for y in yticks]))

# Remove minor ticks
ax.xaxis.set_minor_locator(FixedLocator([]))
ax.yaxis.set_minor_locator(FixedLocator([]))

ax.set_xlabel('Operational Intensity (FLOP / byte)')
ax.set_ylabel('Performance (GFLOP/s)')

plt.savefig("roofline.pdf", format='pdf', bbox_inches='tight')
plt.close()