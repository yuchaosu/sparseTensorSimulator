#!/usr/bin/env python3
"""End-to-end U = exp(-iH dt) build speedup at 14 qubits, 128x128 grid.

Same physics as end_to_end_speedup.py (QuTiP expm vs Taylor validation, real CPU
Taylor timing) but reads the 128x128 cycle results:
  DIAMOND-L3  <- result/ablation_128.csv   (zeroskip=1 cbalance=1, compute_cycles)
  Trapezoid-HS<- result/compare_128.csv     (traphs_cyc, measured @128x128)
  TPU (dense) <- result/tpu_analytical_128.csv (analytical @128x128)
Cycles at 1 GHz. Flexagon is separate (STONNE cycle sim).
"""
import os, csv, sys, argparse
import numpy as np

# reuse the validated physics from the q14/8x8 driver
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import end_to_end_speedup as e2e
from end_to_end_speedup import read_dia, taylor_U, ground_truth_U, DT, HAM, HAVE_QUTIP, fmt

REPO = "/home/ysu34/DIAMOND/diamond"
ABL = f"{REPO}/result/ablation_128.csv"
CMP = f"{REPO}/result/compare_128.csv"
TPU = f"{REPO}/result/tpu_analytical_128.csv"

# (folder, file, q, K) — folders match where the q14 DIA matrices actually live
FAMILIES = [
    ("dia_oom",      "tfim_14_4.txt",    14, 4),
    ("dia_families", "reg3_14_4.txt",    14, 4),
    ("dia_families", "maxcut_14_5.txt",  14, 5),
    ("dia_families", "qmaxcut_14_4.txt", 14, 4),
    ("dia_families", "fermi_14_4.txt",   14, 4),
    ("dia_oom",      "sweepheis_14_5.txt", 14, 5),
    ("dia_families", "chem_14_4.txt",    14, 4),
]


def _key(s):
    return s.removesuffix(".txt")


def load_cycles():
    """{stem: {'diamond':c, 'hs':c_or_None, 'tpu':c_or_None}} from the 128x128 CSVs."""
    out = {}
    # DIAMOND-L3: ablation row zeroskip=1(col22) cbalance=1(col23), compute_cycles=col8
    if os.path.exists(ABL):
        for row in csv.reader(open(ABL)):
            if len(row) < 24 or row[0] == "matrix":
                continue
            if row[22] == "1" and row[23] == "1":
                c = row[8]
                out.setdefault(_key(row[0]), {})["diamond"] = (
                    float(c) if c not in ("", "TIMEOUT", "OOM", "TIMEOUT/OOM") else None)
    # Trapezoid-HS measured @128x128
    if os.path.exists(CMP):
        for r in csv.DictReader(open(CMP)):
            k = _key(r["workload"])
            v = r.get("traphs_cyc", "")
            out.setdefault(k, {})["hs"] = float(v) if v not in ("", "-", "OOM", "FAIL", "TIMEOUT") else None
    # TPU analytical @128x128
    if os.path.exists(TPU):
        for r in csv.DictReader(open(TPU)):
            k = _key(r["workload"])
            v = r.get("tpu_cyc", "")
            out.setdefault(k, {})["tpu"] = float(v) if v not in ("", "-") else None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock-ghz", type=float, default=1.0)
    ap.add_argument("--out", default=f"{REPO}/result/end_to_end_128.csv")
    a = ap.parse_args()
    print(f"# QuTiP {'available '+e2e.qutip.__version__ if HAVE_QUTIP else 'NOT installed -> scipy expm fallback'}")
    print(f"# dt={DT}  clock={a.clock_ghz} GHz  grid=128x128")

    cyc = load_cycles()

    # ---- correctness: expm vs Taylor at small q per family ----
    print("\n== correctness (expm vs Taylor) ==")
    import glob, re
    for folder, fil, q, K in FAMILIES:
        base = fil[:-4].rsplit("_", 2)[0]
        cands = []
        for cf in ("dia_families", "dia_e2e", "dia_oom"):
            for p in glob.glob(f"{HAM}/{cf}/{base}_*_*.txt"):
                m = re.search(rf"{re.escape(base)}_(\d+)_(\d+)\.txt$", os.path.basename(p))
                if m:
                    cands.append((int(m.group(1)), int(m.group(2)), p))
        cands.sort()
        pick = next((c for c in cands if c[0] <= 10), cands[0] if cands else None)
        if not pick:
            print(f"  {base:<10}: no validation file"); continue
        vq, vK, vpath = pick
        H, n = read_dia(vpath)
        Uref, _ = ground_truth_U(H, DT)
        Utay, _, _ = taylor_U(H, DT, vK)
        err = float(np.max(np.abs(Utay.toarray() - Uref)))
        print(f"  {base:<10} q{vq} K{vK}: max|U_taylor-U_expm|={err:.2e} {'OK' if err<1e-6 else 'CHECK'}")

    # ---- end-to-end timing + speedup @ q14, 128x128 ----
    print(f"\n== end-to-end U build @14q, 128x128 (clock {a.clock_ghz} GHz) ==")
    hdr = ["family", "n", "K", "diamond_cyc", "t_diamond_s", "hs_cyc", "tpu_cyc",
           "t_cpu_taylor_s", "spdup_vs_cpu", "spdup_vs_hs", "spdup_vs_tpu", "cpu_note"]
    rows = []
    for folder, fil, q, K in FAMILIES:
        stem = fil[:-4]
        path = f"{HAM}/{folder}/{fil}"
        if not os.path.exists(path):
            print(f"  {stem:<14} MISSING {path}"); continue
        H, n = read_dia(path)
        info = cyc.get(stem, {})
        dcyc = info.get("diamond")
        t_d = dcyc / (a.clock_ghz * 1e9) if dcyc else None
        try:
            _, t_cpu, last = taylor_U(H, DT, K)
            cpu_note = f"last_term~{last:.1e}"
        except Exception as ex:
            t_cpu, cpu_note = None, f"cpu_failed:{type(ex).__name__}"
        hs_c = info.get("hs")
        tpu_c = info.get("tpu")
        sp_cpu = (t_cpu / t_d) if (t_cpu and t_d) else None
        sp_hs = (hs_c / dcyc) if (hs_c and dcyc) else None
        sp_tpu = (tpu_c / dcyc) if (tpu_c and dcyc) else None
        rows.append([stem, n, K, dcyc, t_d, hs_c, tpu_c, t_cpu, sp_cpu, sp_hs, sp_tpu, cpu_note])
        print(f"  {stem:<14} n={n} K={K} | DIAMOND {fmt(dcyc)} cyc = {fmt(t_d)}s | "
              f"CPU {fmt(t_cpu)}s | vs CPU {fmt(sp_cpu)}x vs HS {fmt(sp_hs)}x vs TPU {fmt(sp_tpu)}x")

    with open(a.out, "w", newline="") as f:
        w = csv.writer(f); w.writerow(hdr)
        for r in rows:
            w.writerow(["" if x is None else x for x in r])
    print(f"\nwrote {a.out}")


if __name__ == "__main__":
    main()
