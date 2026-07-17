#!/usr/bin/env python3
"""Generate the 16x16 sweep manifests from the base 32 workloads (manifest_fast.txt) plus the
newly-converted dia_expand workloads (read from their .meta files).

Writes:
  isca/manifest_16x16.txt       mesh tasks:  method|folder|file|q|K|cls
                                 method in diamond_l1,diamond_l2,diamond_l3,tpu,traphs
  isca/manifest_16x16_flex.txt  flexagon:    impl|folder|file|q|K|cls   (q==14 only; op,gustavson)
"""
import glob, os

REPO = "/home/ysu34/DIAMOND/diamond"
HAM = "/mnt/beegfs/ysu34/hamlib"
EXPAND = f"{HAM}/dia_expand"
FAM_CLASS = {"bh": "fill-in", "tsp": "special", "O2": "fill-in", "Li2": "fill-in",
             "chem": "fill-in", "c2h": "fill-in", "hnc": "fill-in"}
MESH_METHODS = ["diamond_l1", "diamond_l2", "diamond_l3", "tpu", "traphs"]


def base_workloads():
    """distinct (folder,file,q,K,cls) from manifest_fast.txt (drops the method col)."""
    seen, out = set(), []
    with open(f"{REPO}/isca/manifest_fast.txt") as f:
        for ln in f:
            p = ln.strip().split("|")
            if len(p) < 6:
                continue
            key = tuple(p[1:6])            # folder,file,q,K,cls
            if key not in seen:
                seen.add(key); out.append(list(key))
    return out


def meta(path):
    d = {}
    for ln in open(path):
        k, _, v = ln.strip().partition(" ")
        d[k] = v
    return d


def expand_workloads():
    out = []
    for m in sorted(glob.glob(f"{EXPAND}/*.txt.meta")):
        d = meta(m)
        fname = os.path.basename(m)[:-5]           # strip .meta -> <fam>_<q>_<K>.txt
        fam = fname.split("_")[0]
        cls = FAM_CLASS.get(fam, "fill-in")
        out.append(["dia_expand", fname, d["num_qubits"], d["n_taylor"], cls])
    return out


def main():
    wls = base_workloads() + expand_workloads()
    # dedup (folder,file) in case of overlap
    seen, uniq = set(), []
    for w in wls:
        if (w[0], w[1]) not in seen:
            seen.add((w[0], w[1])); uniq.append(w)

    with open(f"{REPO}/isca/manifest_16x16.txt", "w") as f:
        for meth in MESH_METHODS:
            for folder, file, q, K, cls in uniq:
                f.write(f"{meth}|{folder}|{file}|{q}|{K}|{cls}\n")

    with open(f"{REPO}/isca/manifest_16x16_flex.txt", "w") as f:
        for folder, file, q, K, cls in uniq:
            if int(q) == 14:                        # Flexagon only runs q=14 (STONNE dim ceiling at q16)
                for impl in ("op", "gustavson"):
                    f.write(f"{impl}|{folder}|{file}|{q}|{K}|{cls}\n")

    nq14 = sum(1 for w in uniq if int(w[2]) == 14)
    print(f"workloads: {len(uniq)} (q14={nq14})")
    print(f"mesh manifest: {len(uniq)*len(MESH_METHODS)} tasks")
    print(f"flex manifest: {nq14*2} tasks")


if __name__ == "__main__":
    main()
