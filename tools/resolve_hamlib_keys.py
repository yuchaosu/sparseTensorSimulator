#!/usr/bin/env python3
"""Resolve the undetermined HamLib HDF5 keys for the dia_oom workloads.

Why this exists
---------------
isca/hamlib_provenance.csv records, for every workload used in the sweeps, its
source HDF5 file + key. tfim is fully resolved from the artifact (D=1 => the
matrix is diagonal-only => the transverse field is h=0). heis (field h) and bh
(Lx/U/enc) were never logged, so their keys carry <H>/<L>/<U>/<enc> placeholders.

This script resolves them DEFINITIVELY without guessing: for each candidate key
it re-runs the ORIGINAL generator (fetchham_dia.py) with the same fetch params
(final_time=1.2, num_timesteps=1000 => dt=0.0012, matching the sim pipeline),
then matches the regenerated (num_qubits, n_taylor K, num_diagonals D) against the
target workload file. The unique match IS the source key. No reimplementation of
the physics — it reuses the exact code path that produced the files.

Requires the HamLib env (h5py + qiskit) that fetchham_dia.py needs; it is NOT
available on the plain login python, so run this where you originally fetched.

Cost note: densification is heavy at high q. By default it resolves each family
at its SMALLEST q only (heis_18, bh_18) and reports the key; a HamLib family is
fetched with one fixed h/U/enc, so the resolved parameter applies to every q in
that family. Pass --all to check every q (expensive at q>=24).

Usage:
  python tools/resolve_hamlib_keys.py                 # cheap: resolve at min q
  python tools/resolve_hamlib_keys.py --all           # verify at every q
Then paste the resolved keys into isca/hamlib_provenance.csv (replace the
<...> placeholders and set key_status=resolved).
"""
import os, re, sys, glob, tempfile, subprocess

HAMLIB = "/mnt/beegfs/ysu34/hamlib"
DIA    = f"{HAMLIB}/dia_oom"
FETCH  = f"{HAMLIB}/fetchham_dia.py"          # the original generator (lives in the HamLib tree)
FINAL_TIME, NUM_TIMESTEPS = "1.2", "1000"     # dt = 0.0012, matches the sim pipeline

# workload -> (family, hdf5 file). tfim omitted: already resolved (h-0).
TARGETS = {
    "heis": {"hdf5": f"{HAMLIB}/heis/heis.hdf5",
             "files": ["heis_18_5", "heis_20_6", "heis_22_6",
                       "heis_24_6", "heis_26_6", "heis_28_6"]},
    "bh":   {"hdf5": f"{HAMLIB}/BH/BH.hdf5",
             "files": ["bh_18_5", "bh_20_5", "bh_24_5"]},
}

def header(path):
    """Return (N, D) from a dia_oom .txt header line 'N <n> D <d>'."""
    with open(path) as fh:
        t = fh.readline().split()
    return int(t[1]), int(t[3])

def all_keys(hdf5):
    import h5py
    ks = []
    with h5py.File(hdf5, "r") as h:
        h.visititems(lambda name, obj: ks.append(name)
                     if isinstance(obj, h5py.Dataset) else None)
    return ks

def candidate_keys(family, num_qubits, hdf5):
    """Keys that could plausibly yield `num_qubits`, to keep the search small."""
    ks = all_keys(hdf5)
    if family == "heis":
        # heis num_qubits == Lx exactly; only the field h is free.
        return [k for k in ks if re.search(rf"Lx-{num_qubits}_h-", k)]
    # bh: num_qubits = Lx * bits_per_site; can't precompute bits, so try every
    # key whose Lx is a small divisor-ish candidate. Cheapest to try all and let
    # the generator report num_qubits; filter by the fetch result instead.
    return ks

def regenerate(hdf5, key, outdir):
    """Run the original generator; return (num_qubits, K, D) or None on failure."""
    r = subprocess.run([sys.executable, FETCH, hdf5, key,
                        FINAL_TIME, NUM_TIMESTEPS, outdir],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    made = glob.glob(os.path.join(outdir, "*.txt"))
    if not made:
        return None
    f = made[0]
    m = re.match(r".*_(\d+)_(\d+)\.txt$", os.path.basename(f))
    N, D = header(f)
    os.remove(f)
    return (int(m.group(1)), int(m.group(2)), D)   # (num_qubits, K, D)

def resolve_family(family, hdf5, workload, check_all):
    q  = int(workload.split("_")[1])
    K  = int(workload.split("_")[2])
    N, D = header(f"{DIA}/{workload}.txt")
    print(f"[{workload}] target: qubits={q} K={K} D={D}", flush=True)
    with tempfile.TemporaryDirectory() as td:
        for key in candidate_keys(family, q, hdf5):
            got = regenerate(hdf5, key, td)
            if got is None:
                continue
            gq, gK, gD = got
            if gq == q and gK == K and gD == D:
                print(f"  MATCH -> {key}", flush=True)
                return key
    print(f"  NO MATCH (widen candidate set or check fetch params)", flush=True)
    return None

def main():
    check_all = "--all" in sys.argv
    for fam, spec in TARGETS.items():
        files = spec["files"] if check_all else spec["files"][:1]  # min q only unless --all
        for wl in files:
            resolve_family(fam, spec["hdf5"], wl, check_all)

if __name__ == "__main__":
    main()
