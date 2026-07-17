#!/usr/bin/env python3
"""Build SLURM task manifests for the 128x128 sweep.
Resolves the canonical DIA file per (family, q) across the HamLib DIA folders,
pinning the DIAGONAL (D=1) variant for tfim/reg3/maxcut. Emits two manifests:
  manifest_fast.txt : DIAMOND L1/L2/L3 + Trapezoid-HS   (C++ sims)
  manifest_flex.txt : Flexagon gustavson + op            (STONNE, slow, ramulator2)
Each line: method|folder|file|q|K|class
TPU is analytical (result/tpu_analytical_128.csv), not a sim task.
"""
import os, re, glob

HAM = "/mnt/beegfs/ysu34/hamlib"
FOLDERS = ["dia_families", "dia_oom", "dia_e2e"]        # search order
QS = [14, 16, 18, 20]

# family -> (class, force_diagonal). force_diagonal picks the D=1 file (tfim has a banded twin).
FAMILIES = {
    "reg3":      ("diagonal", True),
    "maxcut":    ("diagonal", True),
    "tfim":      ("diagonal", True),
    "qmaxcut":   ("banded",   False),
    "fermi":     ("banded",   False),
    "heis":      ("banded",   False),
    "sweepheis": ("banded",   False),
    "chem":      ("fill-in",  False),
    "tsp":       ("special",  False),
    "bh":        ("fill-in",  False),
}


def matrix_D(path):
    with open(path) as f:
        m = re.search(r"D\s+(\d+)", f.readline())
        return int(m.group(1)) if m else -1


def resolve(fam, q, force_diag):
    """Return (folder, file, K) for the canonical (fam,q) DIA file, or None."""
    cands = []
    for fol in FOLDERS:
        for p in glob.glob(f"{HAM}/{fol}/{fam}_{q}_*.txt"):
            mm = re.search(rf"{fam}_{q}_(\d+)\.txt$", os.path.basename(p))
            if not mm:
                continue
            cands.append((fol, os.path.basename(p), int(mm.group(1)), matrix_D(p)))
    if not cands:
        return None
    if force_diag:
        diag = [c for c in cands if c[3] == 1]
        if diag:
            cands = diag
    # prefer dia_families, then dia_oom, then dia_e2e (FOLDERS order)
    cands.sort(key=lambda c: FOLDERS.index(c[0]))
    fol, fil, K, D = cands[0]
    return fol, fil, K


def main():
    outdir = os.path.dirname(os.path.abspath(__file__))
    fast, flex, table = [], [], []
    for fam, (cls, fd) in FAMILIES.items():
        for q in QS:
            r = resolve(fam, q, fd)
            if not r:
                continue
            fol, fil, K = r
            table.append((fam, q, K, cls, fol, fil))
            # compare: DIAMOND-L3 + Trapezoid-HS at every (fam,q)
            fast.append(f"diamond_l3|{fol}|{fil}|{q}|{K}|{cls}")
            fast.append(f"traphs|{fol}|{fil}|{q}|{K}|{cls}")
            # Flexagon gus + op at every (fam,q)
            flex.append(f"flexgus|{fol}|{fil}|{q}|{K}|{cls}")
            flex.append(f"flexop|{fol}|{fil}|{q}|{K}|{cls}")
            # ablation: L1/L2 only at q14 (L3 already in fast)
            if q == 14:
                fast.append(f"diamond_l1|{fol}|{fil}|{q}|{K}|{cls}")
                fast.append(f"diamond_l2|{fol}|{fil}|{q}|{K}|{cls}")
    with open(f"{outdir}/manifest_fast.txt", "w") as f:
        f.write("\n".join(fast) + "\n")
    with open(f"{outdir}/manifest_flex.txt", "w") as f:
        f.write("\n".join(flex) + "\n")
    print(f"resolved {len(table)} workloads:")
    for fam, q, K, cls, fol, fil in table:
        print(f"  {fam:<10} q{q:<3} K{K} {cls:<8} {fol}/{fil}")
    print(f"\nmanifest_fast.txt: {len(fast)} tasks (DIAMOND L1/L2/L3 + Trapezoid-HS)")
    print(f"manifest_flex.txt: {len(flex)} tasks (Flexagon gus+op, ramulator2)")


if __name__ == "__main__":
    main()
