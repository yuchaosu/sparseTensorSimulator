#!/usr/bin/env python3
"""
Summarize PE compute trace file.
Input columns: timestamp_ns,pe_r,pe_c,evt,flops,idx1,idx2,result
Outputs: CSV per-PE flops and counts
Usage: python3 compute_summary.py compute_trace.csv > compute_summary.csv
"""
import sys
from collections import defaultdict

if len(sys.argv) < 2:
    print("Usage: compute_summary.py TRACE_CSV")
    sys.exit(1)

path = sys.argv[1]
per = defaultdict(lambda: {'flops':0,'count':0})

with open(path) as f:
    header = f.readline()
    for line in f:
        line=line.strip()
        if not line: continue
        parts = line.split(',')
        if len(parts) < 8: continue
        # timestamp_ns,pe_r,pe_c,evt,flops,idx1,idx2,result
        pe_r = int(parts[1]); pe_c = int(parts[2])
        evt = parts[3]
        flops = int(parts[4])
        key = (pe_r,pe_c)
        if evt == 'COMPUTE':
            per[key]['flops'] += flops
            per[key]['count'] += 1

print('pe_r,pe_c,total_flops,events')
for (r,c),v in sorted(per.items()):
    print(f"{r},{c},{v['flops']},{v['count']}")
