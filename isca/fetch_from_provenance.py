#!/usr/bin/env python3
"""Provenance-driven HamLib fetch + DIA build.

Single source of truth = hamlib_provenance.csv. For each row it (1) resolves the
HamLib .hdf5 (local under --ham, else downloads the family zip from the HamLib
portal), (2) reads the Hamiltonian by its exact `hdf5_key`, and (3) builds the
DIA `.txt` DIAMOND consumes ("N <n> D <d>" then one line per nonzero diagonal
"<off>: v0 v1 ..." of length n-|off|).

Unlike the old helper/fetchham.py (dense `to_matrix()`, hardcoded paths, argv), this
builds the operator SPARSELY (SparsePauliOp.to_matrix(sparse=True)) so large q is
feasible, and is fully driven by the provenance columns (family, hdf5_file, hdf5_key,
num_qubits). Raw hdf5 are treated as immutable; DIA output goes to --outdir (a derived
build dir), never overwriting the source.

Usage:
  python3 fetch_from_provenance.py                       # build all buildable rows
  python3 fetch_from_provenance.py --workload tfim_14_4  # one row
  python3 fetch_from_provenance.py --verify              # build + check N/D/nnz vs provenance
"""
import argparse, csv, glob, os, re, sys, zipfile, urllib.request
import numpy as np
import scipy.sparse as sp

REPO = "/home/ysu34/DIAMOND/diamond"
HAM = "/mnt/beegfs/ysu34/hamlib"
PORTAL = "https://portal.nersc.gov/cfs/m888/dcamps/hamlib"
# family -> portal category/subdir holding <name>.zip -> <name>.hdf5. Only the
# CONFIRMED categories are listed; unlisted families must be fetched manually
# (we refuse to guess a URL). chemistry molecule paths are left unset on purpose.
PORTAL_DIR = {
    "tfim": "condensedmatter/tfim",       "heis": "condensedmatter/heisenberg",
    "fermi": "condensedmatter/fermihubbard", "bh": "condensedmatter/bosehubbard",
    "maxcut": "binaryoptimization/maxcut",
    "reg3": "binaryoptimization/maxcut",  "tsp": "discreteoptimization/tsp",
}
# NOTE: PORTAL_DIR is only a fallback -- the provenance `portal_path` column is the source
# of truth and overrides it for every family that has one. qmaxcut/chem/Li2/O2 have no
# portal_path (locally assembled) and are intentionally NOT here, so --download reports a
# clean "fetch manually" error for them instead of chasing a guessed (404) URL.


# ---- HamLib Pauli-string reader (from helper/fetchham.py, self-contained) ----
def read_sparse_pauli(fname_hdf5, key):
    from qiskit.quantum_info import SparsePauliOp
    def _gen(term):
        idx = [(m.group(1), int(m.group(2))) for m in re.finditer(r'([A-Z])(\d+)', term)]
        n = max(i for _, i in idx) + 1
        return ''.join(next((c for c, i in idx if i == k), 'I') for k in range(n))
    def _pad(ps):
        w = max(map(len, ps))
        return [p + 'I' * (w - len(p)) for p in ps]
    import h5py
    with h5py.File(fname_hdf5, 'r') as f:
        if key not in f:
            raise KeyError(f"key not in hdf5: {key}")
        txt = f[key][()].decode("utf-8")
    pat = r'\(?([\d.-]+(?:[+-][\d.]+j)?)\)? \[([^\]]+)\]'
    m = re.findall(pat, txt)
    labels = [_gen(x[1]) for x in m]
    coeffs = [complex(x[0]).real for x in m]
    return SparsePauliOp(_pad(labels), coeffs)


def resolve_hdf5(row, ham, download):
    """Return a local path to the row's hdf5: search --ham first; else download the
    family zip from the portal and unzip. Raises if unresolved (never guesses a URL).

    The download subdir comes from the provenance `portal_path` column (the source of
    truth), falling back to PORTAL_DIR only if that column is blank. `hdf5_file` may be a
    bare name or a legacy absolute path -- we always key on its basename."""
    fname = os.path.basename(row["hdf5_file"])              # tolerate legacy /mnt/.../x.hdf5
    hits = glob.glob(os.path.join(ham, "**", fname), recursive=True)
    if hits:
        return hits[0]
    if not download:
        raise FileNotFoundError(f"{fname} not found under {ham} (use --download to fetch)")
    fam = row["family"]
    sub = (row.get("portal_path") or "").strip()
    if sub.startswith("/") or sub.startswith("http"):       # not a portal-relative subdir
        sub = ""
    if not sub:
        sub = PORTAL_DIR.get(fam, "")
    if not sub:
        raise FileNotFoundError(
            f"no portal_path in provenance for family '{fam}' (hdf5 {fname}) — this source "
            f"was locally assembled; fetch it manually or add its portal_path to the CSV")
    base = fname[:-5] if fname.endswith(".hdf5") else fname  # <name>.hdf5 -> <name>
    url = f"{PORTAL}/{sub}/{base}.zip"
    dest_dir = os.path.join(ham, base)
    os.makedirs(dest_dir, exist_ok=True)
    zpath = os.path.join(dest_dir, base + ".zip")
    print(f"    downloading {url}")
    urllib.request.urlretrieve(url, zpath)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(dest_dir)
    hits = glob.glob(os.path.join(ham, "**", fname), recursive=True)
    if not hits:
        raise FileNotFoundError(f"downloaded {url} but {fname} not inside the zip")
    return hits[0]


def build_dia(Hsp, out_path):
    """Write a scipy sparse H as DIA text. Returns (n, num_diagonals, nnz)."""
    H = Hsp.tocoo()
    n = H.shape[0]
    diags = {}                                   # offset -> {row_index_along_diag: value}
    for r, c, v in zip(H.row, H.col, H.data):
        if v != 0.0:
            off = int(c) - int(r)
            k = int(r) if off >= 0 else int(c)   # position along the diagonal (0-based)
            diags.setdefault(off, {})[k] = float(v)
    offs = sorted(diags)
    nnz = sum(len(d) for d in diags.values())
    with open(out_path, "w") as f:
        f.write(f"N {n} D {len(offs)}\n")
        for off in offs:
            length = n - abs(off)
            d = diags[off]
            # full diagonal (zeros included) so positions align with the DIA loader
            f.write(f"{off}: " + " ".join(str(d.get(k, 0.0)) for k in range(length)) + "\n")
    return n, len(offs), nnz


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provenance", default=f"{REPO}/isca/hamlib_provenance.csv")
    ap.add_argument("--ham", default=HAM)
    ap.add_argument("--outdir", default=f"{HAM}/dia_from_provenance")
    ap.add_argument("--workload", default=None, help="build only this workload_file (stem or with .txt)")
    ap.add_argument("--max-qubits", type=int, default=22, help="skip rows above this (dense build infeasible)")
    ap.add_argument("--download", action="store_true", help="download from the portal if hdf5 missing")
    ap.add_argument("--verify", action="store_true", help="compare built N/D/nnz against provenance")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    rows = list(csv.DictReader(open(a.provenance)))
    if a.workload:
        w = a.workload if a.workload.endswith(".txt") else a.workload + ".txt"
        rows = [r for r in rows if r["workload_file"] == w]
        if not rows:
            sys.exit(f"no provenance row for {w}")

    built = skipped = failed = 0
    for r in rows:
        wl = r["workload_file"]
        if r.get("key_confidence") == "dropped":
            print(f"[skip] {wl}: dropped in provenance"); skipped += 1; continue
        q = int(r["num_qubits"]) if r["num_qubits"] else 0
        if q > a.max_qubits:
            print(f"[skip] {wl}: q={q} > max_qubits={a.max_qubits}"); skipped += 1; continue
        try:
            hpath = resolve_hdf5(r, a.ham, a.download)
            op = read_sparse_pauli(hpath, r["hdf5_key"])
            Hsp = op.to_matrix(sparse=True).real
            out = os.path.join(a.outdir, wl)
            n, D, nnz = build_dia(sp.csr_matrix(Hsp), out)
            note = ""
            if a.verify:
                pN, pD, pnnz = r["matrix_dim_N"], r["num_diagonals_D"], r.get("nnz", "")
                ok = (str(n) == pN) and (str(D) == pD) and (pnnz == "" or str(nnz) == pnnz)
                note = f"  vs prov(N={pN},D={pD},nnz={pnnz}) {'OK' if ok else 'MISMATCH'}"
            print(f"[built] {wl}: N={n} D={D} nnz={nnz} -> {out}{note}")
            built += 1
        except Exception as e:
            print(f"[FAIL] {wl}: {type(e).__name__}: {e}"); failed += 1
    print(f"\nbuilt={built} skipped={skipped} failed={failed}  outdir={a.outdir}")


if __name__ == "__main__":
    main()
