#!/usr/bin/env python3
"""Merge heavy-build SLURM results (isca/heavy_rows/build_*.csv 'RESULT,...' lines)
into hamlib_provenance.csv. Adds/updates the family-coverage rows built off the login
node (vib_18/vib_20). OK -> full verified row; OOM/TIMEOUT -> a 'dropped' row noting
the fill-in infeasibility (consistent with O2_20). Run after job 259560 finishes."""
import csv, glob, os, re

REPO = "/home/ysu34/DIAMOND/diamond"
PROV = f"{REPO}/isca/hamlib_provenance.csv"
ROWS = f"{REPO}/isca/heavy_rows"
# family -> (portal_path, hdf5 per q, key per q) for the heavy instances
META = {
    ("vib", 18): ("chemistry/vibrational", "all-vib-h2cc.hdf5", "enc_gray_dvalues_8-8-8-8-8-8"),
    ("vib", 20): ("chemistry/vibrational", "all-vib-bhf2.hdf5", "enc_gray_dvalues_16-16-8-8-8-8"),
}


def mtype(D):
    D = int(D)
    return "single-diagonal" if D == 1 else ("banded" if D <= 64 else "fill-in")


def main():
    results = {}
    for f in glob.glob(f"{ROWS}/build_*.csv"):
        for ln in open(f):
            if ln.startswith("RESULT,"):
                p = ln.strip().split(",")
                results[(p[1], int(p[2]))] = p  # (family,q) -> [RESULT,fam,q,N,D,nnz,status]
    if not results:
        print("no RESULT rows found in", ROWS); return

    rows = list(csv.DictReader(open(PROV)))
    cols = list(rows[0].keys())
    have = {r["workload_file"] for r in rows}
    cat = {"chemistry"}
    added = 0
    for (fam, q), p in sorted(results.items()):
        wl = f"{fam}_{q}_4.txt"
        if wl in have:
            continue
        _, _, _, N, D, nnz, status = p
        pp, hdf5, key = META.get((fam, q), ("", "", ""))
        r = {c: "" for c in cols}
        r.update(dict(workload_file=wl, family=fam, hamlib_category=pp.split("/")[0],
                      num_qubits=str(q), hdf5_file=hdf5, hdf5_key=key, portal_path=pp,
                      fetch_script="build_dia_job.py"))
        if status == "OK":
            r.update(dict(structural_class=mtype(D), matrix_type=mtype(D), matrix_dim_N=N,
                          num_diagonals_D=D, nnz=nnz,
                          sparsity=f"{int(nnz)/int(N)**2:.3e}",
                          diagonal_sparsity=f"{int(D)/(2*int(N)-1):.3e}",
                          n_taylor_K="4", key_confidence="verified",
                          key_status="RESOLVED (portal-verified; family coverage q14-20)"))
        else:
            r.update(dict(key_confidence="dropped",
                          key_status=f"DROPPED: fill-in {status} at q{q} (assembly infeasible)"))
        rows.append({c: str(r.get(c, "")) for c in cols})
        added += 1
        print(f"merged {wl}: status={status} N={N} D={D} nnz={nnz}")
    with open(PROV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols); w.writeheader(); w.writerows(rows)
    print(f"added {added} rows; provenance now {len(rows)} data rows")


if __name__ == "__main__":
    main()
