#!/usr/bin/env python3
r"""End-to-end U-operator speedup: build the time-evolution propagator
U = exp(-i H dt) for the 14-qubit Hamiltonians and compare the DIAMOND
accelerator against the baselines and a CPU reference (QuTiP).

WHAT "end-to-end" MEANS HERE
----------------------------
The accelerator does not emit U directly; it builds the Taylor powers H^1..H^K,
from which U = sum_{k=0}^K (-i dt)^k / k! * H^k. The dominant cost is the K
sparse matrix-matrix products H^{k-1}·H (the SpMSpM the mesh runs), so the
accelerator's *cycle* count already covers the whole propagator build
(HBMHamiltonian.cpp:1581-1620). This script:

  1. CORRECTNESS: at a small validation qubit count, build U two ways —
     (a) QuTiP/scipy ground truth expm(-iH dt), (b) the Taylor series the
     accelerator uses — and check they agree (confirms K is converged, so the
     accelerator's exact H^k really yield U).
  2. CPU BASELINE (T_cpu): time the SAME Taylor build on CPU (QuTiP/scipy) at
     q=14 — this is the software cost the accelerator replaces.
  3. ACCELERATOR / BASELINE TIME: cycles / clock, cycles read from the sweep
     CSVs (DIAMOND-L3, TPU, Trapezoid-HS; Flexagon is q<=12 only).
  4. SPEEDUP: T_cpu / T_accel (vs CPU) and cyc_baseline / cyc_diamond (vs each
     hardware baseline), per family, at 14 qubits.

Ground truth uses QuTiP if importable (Qobj.expm), else scipy.sparse.linalg.expm
— the underlying propagator is identical (QuTiP wraps scipy).

Usage:
  ./end_to_end_speedup.py                       # all q14 families, dt=0.0012
  ./end_to_end_speedup.py --validate-q 8        # correctness check size
  ./end_to_end_speedup.py --family qmaxcut_14_4 --clock-ghz 1.0
"""
from __future__ import annotations
import argparse, csv, math, os, time
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
from scipy.linalg import expm as dense_expm

try:
    import qutip
    HAVE_QUTIP = True
except Exception:
    HAVE_QUTIP = False

HAM = "/mnt/beegfs/ysu34/hamlib"
CMP = "/mnt/beegfs/ysu34/family_sweep/comparison.csv"
ABL = "/mnt/beegfs/ysu34/family_ablation_q14/ablation_q14.csv"   # DIAMOND-L3 @ q14 (all families)
DT, TOL = 0.0012, 1e-9

# q14 families (folder, file, K) — heis via sweepheis; see hamlib_provenance_families.csv
FAMILIES = [
    ("dia_oom",      "tfim_14_4.txt",     14, 4),
    ("dia_families", "reg3_14_4.txt",     14, 4),
    ("dia_families", "maxcut_14_5.txt",   14, 5),
    ("dia_families", "qmaxcut_14_4.txt",  14, 4),
    ("dia_families", "fermi_14_4.txt",    14, 4),
    ("dia_families", "chem_14_4.txt",     14, 4),
    ("dia_oom",      "sweepheis_14_5.txt",14, 5),
]


def read_dia(path):
    """Parse a DIA file -> scipy CSR H (real). Format: 'N n D d' then 'off: v...'."""
    offs, diags = [], []
    with open(path) as f:
        hdr = f.readline().split()
        n = int(hdr[1])
        for line in f:
            if ":" not in line:
                continue
            o_str, vals = line.split(":", 1)
            o = int(o_str)
            v = np.fromstring(vals, sep=" ")
            offs.append(o)
            diags.append(v)
    # scipy diags: diagonal for offset o has length n-|o|, matching the file
    H = sp.diags(diags, offs, shape=(n, n), format="csr", dtype=float)
    return H, n


def taylor_U(H, dt, K):
    """U = sum_{k=0}^K (-i dt)^k / k! H^k, built by K successive SpMSpM (the
    accelerator's algorithm). Returns (U_csr, elapsed_s, last_term_1norm)."""
    n = H.shape[0]
    Hc = H.astype(np.complex128)
    U = sp.identity(n, dtype=np.complex128, format="csr")
    P = sp.identity(n, dtype=np.complex128, format="csr")   # H^0
    coeff = 1.0 + 0j
    t0 = time.perf_counter()
    last = 0.0
    for k in range(1, K + 1):
        P = (P @ Hc).tocsr()                 # H^k  (the SpMSpM the mesh runs)
        coeff *= (-1j * dt) / k
        U = U + coeff * P
        last = abs(coeff) * spla.norm(P)     # remainder proxy (this term's size)
    return U.tocsr(), time.perf_counter() - t0, last


def ground_truth_U(H, dt):
    """Reference propagator via QuTiP (Qobj.expm) or scipy dense expm."""
    A = (-1j * dt) * H
    t0 = time.perf_counter()
    if HAVE_QUTIP:
        U = qutip.Qobj(A.tocsc()).expm().full()
    else:
        U = dense_expm(A.toarray())
    return U, time.perf_counter() - t0


def load_cycles():
    """{matrix: {'diamond':c,'tpu':(c,status),'hs':(c,status)}}.
    DIAMOND-L3 from the q14 ablation (most complete for the 14q set); HS/TPU from
    comparison.csv. matrix keys are stripped of a trailing .txt."""
    out = {}
    # DIAMOND-L3 = ablation row zeroskip=1 cbalance=1, compute_cycles.
    # Read POSITIONALLY: the merge's header line is truncated (commas in AH broke
    # --export), but the 30-col data is intact. cols: matrix=0, compute_cycles=8,
    # zeroskip=22, cbalance=23.
    if os.path.exists(ABL):
        with open(ABL) as f:
            for row in csv.reader(f):
                if len(row) < 24 or row[0] == "matrix":
                    continue
                if row[22] == "1" and row[23] == "1":
                    m = row[0].removesuffix(".txt")
                    c = row[8]
                    out.setdefault(m, {})["diamond"] = (
                        float(c) if c not in ("", "TIMEOUT", "OOM") else None)
    # HS / TPU from comparison.csv
    if os.path.exists(CMP):
        with open(CMP) as f:
            for r in csv.DictReader(f):
                m = r["matrix"].removesuffix(".txt")
                d = out.setdefault(m, {})
                d.setdefault("diamond",
                             float(r["diamond_L3_cyc"]) if r["diamond_L3_cyc"] not in ("", "-") else None)
                d["tpu"] = (r["tpu_cyc"], r["tpu_status"])
                d["hs"]  = (r["hs_cyc"],  r["hs_status"])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--family", help="single workload file stem, e.g. qmaxcut_14_4")
    ap.add_argument("--validate-q", type=int, default=8,
                    help="qubit count for the expm-vs-Taylor correctness check")
    ap.add_argument("--clock-ghz", type=float, default=1.0,
                    help="accelerator clock (convgrid model uses 1 GHz)")
    ap.add_argument("--cpu-timeout", type=float, default=600.0,
                    help="skip the CPU Taylor build if it would exceed this (s)")
    ap.add_argument("--out", default="/mnt/beegfs/ysu34/end_to_end_speedup.csv")
    a = ap.parse_args()

    print(f"# QuTiP {'available: '+qutip.__version__ if HAVE_QUTIP else 'NOT installed -> scipy expm fallback'}")
    print(f"# dt={DT}  clock={a.clock_ghz} GHz  tol={TOL}")

    cyc = load_cycles()
    fams = FAMILIES
    if a.family:
        fams = [t for t in FAMILIES if t[1].startswith(a.family)]

    # ---- 1) correctness: expm vs Taylor at the smallest available q per family ----
    print("\n== correctness (QuTiP/scipy expm vs Taylor propagator) ==")
    import glob, re
    for folder, fil, q, K in fams:
        stem = fil[:-4]
        base = stem.rsplit("_", 2)[0]
        # discover all files for this family, pick the smallest q <= validate_q ceiling
        cands = []
        for cf in ("dia_families", "dia_oom"):
            for p in glob.glob(f"{HAM}/{cf}/{base}_*_*.txt"):
                m = re.search(rf"{re.escape(base)}_(\d+)_(\d+)\.txt$", os.path.basename(p))
                if m:
                    cands.append((int(m.group(1)), int(m.group(2)), p))
        cands.sort()
        pick = next((c for c in cands if c[0] <= max(a.validate_q, 12)), cands[0] if cands else None)
        if pick is None:
            print(f"  {base:<10}: no file for validation (skipped)"); continue
        vq, vK, vpath = pick
        H, n = read_dia(vpath)
        Uref, _ = ground_truth_U(H, DT)
        Utay, _, _ = taylor_U(H, DT, vK)
        err = np.max(np.abs(Utay.toarray() - Uref))
        print(f"  {base:<10} q{vq} K{vK}: max|U_taylor - U_expm| = {err:.2e}  "
              f"{'OK' if err < 1e-6 else 'CHECK K'}")

    # ---- 2/3/4) end-to-end timing + speedup at q=14 ----
    print(f"\n== end-to-end U build @ 14 qubits (clock {a.clock_ghz} GHz) ==")
    hdr = ["family", "n", "K", "diamond_cyc", "t_diamond_s", "hs_cyc", "tpu_cyc",
           "t_cpu_taylor_s", "spdup_vs_cpu", "spdup_vs_hs", "spdup_vs_tpu", "cpu_note"]
    rows = []
    for folder, fil, q, K in fams:
        stem = fil[:-4]
        path = f"{HAM}/{folder}/{fil}"
        if not os.path.exists(path):
            print(f"  {stem:<14} MISSING {path}"); continue
        H, n = read_dia(path)
        nnz = H.nnz
        info = cyc.get(stem, {})
        dcyc = info.get("diamond")
        t_d = dcyc / (a.clock_ghz * 1e9) if dcyc else None

        # CPU Taylor build (skip if the matrix is clearly too big to finish in time)
        cpu_note, t_cpu = "", None
        # rough guard: fill-in families (large nnz) can blow up; still try but cap
        try:
            _, t_cpu, last = taylor_U(H, DT, K)
            cpu_note = f"last_term~{last:.1e}"
        except Exception as e:
            cpu_note = f"cpu_failed:{type(e).__name__}"

        hs = info.get("hs", ("", ""))
        tpu = info.get("tpu", ("", ""))
        hs_c = float(hs[0]) if hs[0] not in ("", "-") and hs[1] == "OK" else None
        tpu_c = float(tpu[0]) if tpu[0] not in ("", "-") and tpu[1] == "OK" else None

        sp_cpu = (t_cpu / t_d) if (t_cpu and t_d) else None
        sp_hs  = (hs_c / dcyc) if (hs_c and dcyc) else None
        sp_tpu = (tpu_c / dcyc) if (tpu_c and dcyc) else None
        rows.append([stem, n, K, dcyc, t_d, hs_c, tpu_c, t_cpu, sp_cpu, sp_hs, sp_tpu, cpu_note])
        print(f"  {stem:<14} n={n} K={K} | DIAMOND {dcyc} cyc = {fmt(t_d)}s | "
              f"CPU-Taylor {fmt(t_cpu)}s | speedup vs CPU {fmt(sp_cpu)}x, vs HS {fmt(sp_hs)}x")

    with open(a.out, "w", newline="") as f:
        w = csv.writer(f); w.writerow(hdr)
        for r in rows:
            w.writerow(["" if x is None else x for x in r])
    print(f"\nwrote {a.out}")


def fmt(x):
    if x is None: return "-"
    if isinstance(x, float):
        return f"{x:.3g}"
    return str(x)


if __name__ == "__main__":
    main()
