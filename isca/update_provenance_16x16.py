#!/usr/bin/env python3
"""Append provenance rows for the newly-converted dia_expand workloads to hamlib_provenance.csv.

Values come from each workload's .meta (num_qubits, matrix_dim_N=n, num_diagonals_D, n_taylor_K);
the hdf5_file/hdf5_key are the EXACT keys passed to fetchham_sparse_dia.py (no fabrication).
Idempotent: skips workloads already present in the CSV.
"""
import glob, os

REPO = "/home/ysu34/DIAMOND/diamond"
EXPAND = "/mnt/beegfs/ysu34/hamlib/dia_expand"
PROV = f"{REPO}/isca/hamlib_provenance.csv"

# (family, qubits) -> (hdf5_file, hdf5_key) actually used for the conversion
KEYMAP = {
    ("bh", 14):  ("/mnt/beegfs/ysu34/hamlib/BH/BH.hdf5",
                  "bh_graph-1D-grid-nonpbc-qubitnodes_Lx-7_U-2_enc-stdbinary_d-4"),
    ("bh", 16):  ("/mnt/beegfs/ysu34/hamlib/BH/BH.hdf5",
                  "bh_graph-1D-grid-nonpbc-qubitnodes_Lx-8_U-2_enc-stdbinary_d-4"),
    ("tsp", 16): ("/mnt/beegfs/ysu34/hamlib/tsp/tsp.hdf5", "tsppenalty_Ncity-4_enc-unary"),
    ("O2", 16):  ("/mnt/beegfs/ysu34/hamlib/O2/O2.hdf5", "ham_BK16"),
    ("O2", 20):  ("/mnt/beegfs/ysu34/hamlib/O2/O2.hdf5", "ham_BK20"),
    ("Li2", 14): ("/mnt/beegfs/ysu34/hamlib/chemistry/Li2/Li2.hdf5", "ham_BK14"),
}
FAM_CLASS = {"bh": "fill-in", "tsp": "special", "O2": "fill-in", "Li2": "fill-in"}
# canonical family label in provenance (BH hdf5 -> bh, etc.); tag already lowercase-ish
FAM_LABEL = {"bh": "bh", "tsp": "tsp", "O2": "O2", "Li2": "Li2"}


def meta(path):
    d = {}
    for ln in open(path):
        k, _, v = ln.strip().partition(" ")
        d[k] = v
    return d


def main():
    existing = set()
    with open(PROV) as f:
        header = f.readline().rstrip("\n")
        for ln in f:
            existing.add(ln.split(",", 1)[0])

    rows = []
    for m in sorted(glob.glob(f"{EXPAND}/*.txt.meta")):
        fname = os.path.basename(m)[:-5]            # <fam>_<q>_<K>.txt
        fam = fname.split("_")[0]
        if fam not in FAM_CLASS:                     # c2h/hnc etc. never got written; skip
            continue
        d = meta(m)
        q = int(d["num_qubits"])
        if (fam, q) not in KEYMAP:
            print(f"skip {fname}: no key mapping for ({fam},{q})")
            continue
        if fname in existing:
            print(f"skip {fname}: already in provenance")
            continue
        hdf5, key = KEYMAP[(fam, q)]
        row = [fname, FAM_LABEL.get(fam, fam), FAM_CLASS[fam], str(q), d["n"],
               d["num_diagonals"], d["n_taylor"], hdf5, key,
               f"RESOLVED (expanded 16x16 coverage; D={d['num_diagonals']} nnz={d['nnz']})",
               d.get("final_time", "1.2"), d.get("num_timesteps", "1000"), d.get("dt", "0.0012"),
               "dia-sparse", "fetchham_sparse_dia.py"]
        rows.append(row)

    if not rows:
        print("no new rows to append")
        return
    with open(PROV, "a") as w:
        for r in rows:
            # quote the key_status field (contains no comma here, but be safe on any field w/ comma)
            w.write(",".join(f'"{c}"' if ("," in c) else c for c in r) + "\n")
    print(f"appended {len(rows)} rows:")
    for r in rows:
        print("  " + r[0])


if __name__ == "__main__":
    main()
