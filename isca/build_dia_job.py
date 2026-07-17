#!/usr/bin/env python3
"""Build ONE DIA matrix from a HamLib hdf5 key (for heavy fill-in instances run on a
compute node via SLURM, not the login node). Reuses fetch_from_provenance's reader +
DIA writer. Emits a result line the caller merges into the provenance:
    RESULT,<family>,<q>,<N>,<D>,<nnz>,<status>
status: OK | OOM | ERR:<type>. Does NOT touch hamlib_provenance.csv (race-safe)."""
import argparse, os, sys, resource
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fetch_from_provenance as fp
import scipy.sparse as sp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hdf5", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--q", type=int, required=True)
    ap.add_argument("--family", required=True)
    ap.add_argument("--outdir", default="/mnt/beegfs/ysu34/hamlib/dia_from_provenance")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    wl = f"{a.family}_{a.q}_4.txt"
    try:
        op = fp.read_sparse_pauli(a.hdf5, a.key)
        H = sp.csr_matrix(op.to_matrix(sparse=True).real)   # the memory-heavy step for fill-in
        n, D, nnz = fp.build_dia(H, os.path.join(a.outdir, wl))
        print(f"RESULT,{a.family},{a.q},{n},{D},{nnz},OK", flush=True)
    except MemoryError:
        print(f"RESULT,{a.family},{a.q},,,,OOM", flush=True)
    except Exception as e:
        print(f"RESULT,{a.family},{a.q},,,,ERR:{type(e).__name__}", flush=True)


if __name__ == "__main__":
    main()
