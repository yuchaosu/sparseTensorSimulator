#!/usr/bin/env python3
"""Combine all 16x16 methods into ONE per-workload comparison table (like compare_128.csv).

Reads the SLURM row files in rows16/ and joins by (workload, q, K):
  DIAMOND-L3  <- mesh_*.csv  method=diamond_l3   (dedup: prefer OK over rerun OOM)
  Trapezoid   <- mesh_*.csv  method=traphs
  TPU         <- mesh_*.csv  method=tpu
  Flexagon    <- flex_*.csv  impl=gustavson (flexgus) and impl=op (flexop)
Emits isca/compare_16x16.csv. A method's *_cyc is the cycle count when status==OK,
else "" (its *_status column keeps OOM/TIMEOUT/etc). Speedups are baseline_cyc /
diamond_l3_cyc when both are numeric. Re-run after new results land (e.g. the L3 OOM
reruns) to refresh. sweepheis is excluded (synthetic; .txt deleted)."""
import glob, csv, os

ROWS = "/home/ysu34/DIAMOND/diamond/isca/rows16"
OUT = "/home/ysu34/DIAMOND/diamond/isca/compare_16x16.csv"
EXCLUDE = ("sweepheis",)


def fam(wl):
    return wl.split("_")[0]


def _ok(status):
    return status == "OK"


def load_mesh():
    """(method,wl,q,K) -> (cycles, status), dedup preferring OK."""
    best = {}
    for f in glob.glob(os.path.join(ROWS, "mesh_*.csv")):
        for r in csv.reader(open(f)):
            # method,level,workload,q,K,class,pe,cycles,status,sec
            if len(r) < 9 or r[2].startswith(EXCLUDE):
                continue
            key = (r[0], r[2], r[3], r[4])
            cyc, st = r[7], r[8]
            cur = best.get(key)
            if cur is None or (_ok(st) and not _ok(cur[1])):
                best[key] = (cyc, st, r[5])   # cycles, status, class
    return best


def load_flex():
    """(impl,wl,q,K) -> (cycles_total, status). Reads the flex-schema rows: Flexagon
    (flex_*.csv, impl=gustavson/op) AND SegFold (segfold_*.csv, impl=segfold)."""
    out = {}
    for f in glob.glob(os.path.join(ROWS, "flex_*.csv")) + glob.glob(os.path.join(ROWS, "segfold_*.csv")):
        for r in csv.reader(open(f)):
            # impl,workload,q,K,class,pe,cycles_total,per_step,per_nnz,status,sec
            if len(r) < 11 or r[1].startswith(EXCLUDE):
                continue
            out[(r[0], r[1], r[2], r[3])] = (r[6], r[9], r[4])
    return out


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def main():
    mesh, flex = load_mesh(), load_flex()

    # Union of all (workload,q,K) cells across every method, with class if known.
    cells = {}
    for (m, wl, q, K), (_, _, cls) in mesh.items():
        cells.setdefault((wl, q, K), cls)
    for (impl, wl, q, K), (_, _, cls) in flex.items():
        cells.setdefault((wl, q, K), cls)

    def cell(src, key):  # -> (cyc_if_OK, status)
        v = src.get(key)
        if v is None:
            return "", "-"
        cyc, st = v[0], v[1]
        return (cyc if _ok(st) else ""), st

    hdr = ["workload", "family", "q", "K", "class",
           "diamond_l3_cyc", "flexgus_cyc", "flexop_cyc", "traphs_cyc", "tpu_cyc", "segfold_cyc",
           "diamond_status", "flexgus_status", "flexop_status", "traphs_status", "tpu_status", "segfold_status",
           "spdup_vs_flexgus", "spdup_vs_flexop", "spdup_vs_traphs", "spdup_vs_tpu", "spdup_vs_segfold"]
    rows = []
    for (wl, q, K), cls in sorted(cells.items(), key=lambda x: (fam(x[0][0]), int(x[0][1]), x[0][0])):
        d_c, d_s = cell(mesh, ("diamond_l3", wl, q, K))
        fg_c, fg_s = cell(flex, ("gustavson", wl, q, K))
        fo_c, fo_s = cell(flex, ("op", wl, q, K))
        tr_c, tr_s = cell(mesh, ("traphs", wl, q, K))
        tp_c, tp_s = cell(mesh, ("tpu", wl, q, K))
        sf_c, sf_s = cell(flex, ("segfold", wl, q, K))
        d = num(d_c)
        sp = lambda b: (f"{num(b)/d:.2f}" if (num(b) is not None and d) else "-")
        rows.append([wl, fam(wl), q, K, cls,
                     d_c, fg_c, fo_c, tr_c, tp_c, sf_c,
                     d_s, fg_s, fo_s, tr_s, tp_s, sf_s,
                     sp(fg_c), sp(fo_c), sp(tr_c), sp(tp_c), sp(sf_c)])

    with open(OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(hdr)
        w.writerows(rows)

    ok = sum(1 for r in rows if r[5] != "")
    fx = sum(1 for r in rows if r[6] != "" or r[7] != "")
    print(f"wrote {OUT}: {len(rows)} workloads "
          f"(DIAMOND-L3 OK={ok}, Flexagon cells present={fx})")


if __name__ == "__main__":
    main()
