#!/usr/bin/env python3
"""Parse PrimeTime/pt_shell per-instance CSV and compute PE vs diagonal percentages.

Usage:
    python3 parse_power.py reports/power_instances.csv

This expects a CSV with an instance-name column and a numeric power column. The
script will search for instance names containing '/u_pe' and '/u_diag_accum' and
sum their power values. If your synthesis tool renamed instances, adjust the
regexes at the top of the file.
"""

import csv
import re
import sys
from pathlib import Path

PE_PATTERN = re.compile(r"/u_pe\b")
DIAG_PATTERN = re.compile(r"/u_diag_accum\b")

def find_columns(header):
    inst_idx = None
    power_idx = None
    for i, h in enumerate(header):
        low = h.lower()
        if inst_idx is None and ("instance" in low or "name" in low or "hier" in low):
            inst_idx = i
        if power_idx is None and ("power" in low or "total" in low):
            power_idx = i
    # fallback
    if inst_idx is None:
        inst_idx = 0
    if power_idx is None:
        power_idx = 1
    return inst_idx, power_idx


def parse_csv(path: Path):
    if not path.exists():
        raise SystemExit(f"CSV file not found: {path}")
    with path.open() as fh:
        reader = csv.reader(fh)
        header = next(reader, None)
        if header is None:
            raise SystemExit("Empty CSV")
        inst_idx, power_idx = find_columns(header)

        pe_sum = 0.0
        diag_sum = 0.0
        other_sum = 0.0
        for row in reader:
            if len(row) <= max(inst_idx, power_idx):
                continue
            inst = row[inst_idx]
            val = row[power_idx]
            try:
                p = float(val)
            except Exception:
                # try to strip units if present (e.g., "1.234 mW")
                toks = val.split()
                try:
                    p = float(toks[0])
                except Exception:
                    continue
            if PE_PATTERN.search(inst):
                pe_sum += p
            elif DIAG_PATTERN.search(inst):
                diag_sum += p
            else:
                other_sum += p

    return pe_sum, diag_sum, other_sum


if __name__ == "__main__":
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("reports/power_instances.csv")
    pe, diag, other = parse_csv(path)
    total = pe + diag + other
    print(f"PE total power   : {pe:.6f}")
    print(f"Diag total power : {diag:.6f}")
    print(f"Other total power: {other:.6f}")
    print(f"Total power      : {total:.6f}")
    if total > 0:
        print(f"PE %   = {100.0 * pe / total:.2f}%")
        print(f"Diag % = {100.0 * diag / total:.2f}%")
    else:
        print("Total power is zero, cannot compute percentages")
