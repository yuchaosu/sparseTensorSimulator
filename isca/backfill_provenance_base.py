#!/usr/bin/env python3
"""Backfill provenance rows for the base sweep workloads (q16/18/20 of the core HDF5 families,
plus heis q14/16) that were never documented. Keys are constructed from each family's verified
convention (checked against hdf5_scan.csv; fermi U-2 and heis h-0 confirmed by D-match), D is read
from the existing .txt, N=2^q. sweepheis is EXCLUDED (synthetic, not in any HDF5). Idempotent.
"""
import os, math

REPO = "/home/ysu34/DIAMOND/diamond"
HAM = "/mnt/beegfs/ysu34/hamlib"
PROV = f"{REPO}/isca/hamlib_provenance.csv"

# family -> (hdf5_file, key_template(q), structural_class)
FAM = {
    "tfim":    (f"{HAM}/tfim/tfim.hdf5",                    lambda q: f"graph-1D-grid-nonpbc-qubitnodes_Lx-{q}_h-0",       "diagonal"),
    "maxcut":  (f"{HAM}/maxcut/maxcut.hdf5",                lambda q: f"complbipart-n-{q}_a-{q//2}_b-{q//2}",              "diagonal"),
    "reg3":    (f"{HAM}/maxcut/ham-graph-regular_reg-3.hdf5", lambda q: f"reg-3_n-{q}_rinst-00",                          "diagonal"),
    "qmaxcut": (f"{HAM}/qmaxcut/qmaxcut.hdf5",              lambda q: f"graph-n-{q}",                                      "banded"),
    "fermi":   (f"{HAM}/fermi/fermi.hdf5",                  lambda q: f"fh-graph-1D-grid-nonpbc-qubitnodes_Lx-{q//2}_U-2_enc-jw", "banded"),
    "heis":    (f"{HAM}/heis/heis.hdf5",                    lambda q: f"graph-1D-grid-nonpbc-qubitnodes_Lx-{q}_h-0",       "banded"),
}


def manifest_base():
    """(folder,file,q,K,cls) for the diamond_l3 rows whose family is in FAM (skips dia_expand/sweepheis)."""
    out = []
    with open(f"{REPO}/isca/manifest_16x16.txt") as f:
        for ln in f:
            p = ln.strip().split("|")
            if len(p) < 6 or p[0] != "diamond_l3":
                continue
            folder, file, q, K, cls = p[1:6]
            fam = file.split("_")[0]
            if fam in FAM and folder != "dia_expand":
                out.append((folder, file, int(q), K, cls, fam))
    return out


def count_D(path):
    with open(path) as fh:
        return sum(1 for ln in fh if ":" in ln)


def main():
    existing = set()
    with open(PROV) as f:
        f.readline()
        for ln in f:
            existing.add(ln.split(",", 1)[0])

    rows = []
    for folder, file, q, K, cls, fam in manifest_base():
        if file in existing:
            continue
        txt = f"{HAM}/{folder}/{file}"
        if not os.path.exists(txt):
            print(f"skip {file}: .txt missing at {folder}/")
            continue
        hdf5, keyf, sclass = FAM[fam]
        D = count_D(txt)
        N = 2 ** q
        row = [file, "reg3" if fam == "reg3" else fam, sclass, str(q), str(N), str(D), K,
               hdf5, keyf(q),
               f"RESOLVED (backfilled; convention key verified vs hdf5_scan; D={D})",
               "1.2", "1000", "0.0012", "dia-sparse", "fetchham_sparse_dia.py"]
        rows.append(row)

    if not rows:
        print("no new base rows")
        return
    with open(PROV, "a") as w:
        for r in rows:
            w.write(",".join(f'"{c}"' if ("," in c) else c for c in r) + "\n")
    print(f"appended {len(rows)} base rows:")
    for r in rows:
        print(f"  {r[0]:20s} {r[1]:8s} q{r[3]} K{r[6]} D={r[5]}  {r[8]}")


if __name__ == "__main__":
    main()
