#!/usr/bin/env python3
"""
Summarize HBM trace CSV produced by the HBMController tracing API.
Input trace lines: timestamp_ns,channel,evt,addr,size,requestorId
Outputs per-requestor summary CSV to stdout: requestorId,total_bytes,enq_count,comp_count,first_time_ns,last_time_ns

Usage: python3 trace_summary.py trace.csv > summary.csv
"""
import sys
from collections import defaultdict

if len(sys.argv) < 2:
    print("Usage: trace_summary.py TRACE_CSV")
    sys.exit(1)

path = sys.argv[1]

per = defaultdict(lambda: {
    'total_bytes': 0,
    'enq_count': 0,
    'comp_count': 0,
    'first_time': None,
    'last_time': None
})

with open(path) as f:
    header = f.readline()
    for line in f:
        line = line.strip()
        if not line:
            continue
        parts = line.split(',')
        if len(parts) < 6:
            continue
        ts = int(parts[0])
        # channel = parts[1]
        evt = parts[2]
        addr = parts[3]
        size = int(parts[4])
        req = int(parts[5])

        entry = per[req]
        if entry['first_time'] is None or ts < entry['first_time']:
            entry['first_time'] = ts
        if entry['last_time'] is None or ts > entry['last_time']:
            entry['last_time'] = ts

        if evt == 'ENQ':
            entry['enq_count'] += 1
            entry['total_bytes'] += size
        elif evt == 'COMP':
            entry['comp_count'] += 1
            # optionally include bytes here as well
            entry['total_bytes'] += 0

# Emit CSV
print('requestorId,total_bytes,enq_count,comp_count,first_time_ns,last_time_ns')
for req, v in sorted(per.items()):
    print(f"{req},{v['total_bytes']},{v['enq_count']},{v['comp_count']},{v['first_time']},{v['last_time']}")
