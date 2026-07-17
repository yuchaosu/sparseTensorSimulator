#!/usr/bin/env python3
"""Assemble result/compare_128.csv + result/ablation_128.csv from the SLURM row files.
Row format (rows128/*.csv): method,workload,q,K,class,grid,cycles,status,wall_s
TPU is analytical (result/tpu_analytical_128.csv)."""
import csv, glob, os

REPO = "/home/ysu34/DIAMOND/diamond"
ROWS = f"{REPO}/isca/rows128"
RES = f"{REPO}/result"


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def load_rows():
    d = {}  # (workload,q) -> {method: (cyc,status)}
    meta = {}  # (workload,q) -> (K,class)
    for p in glob.glob(f"{ROWS}/*.csv"):
        for r in csv.reader(open(p)):
            if len(r) < 8:
                continue
            method, wl, q, K, cls, grid, cyc, status = r[:8]
            d.setdefault((wl, q), {})[method] = (cyc, status)
            meta[(wl, q)] = (K, cls)
    return d, meta


def load_tpu():
    t = {}
    p = f"{RES}/tpu_analytical_128.csv"
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            t[(r["workload"], r["qubit"])] = r["tpu_cyc"]
    return t


def fam(wl):
    return wl.rsplit("_", 2)[0]


def main():
    d, meta = load_rows()
    tpu = load_tpu()

    # ---- compare_128.csv ----
    with open(f"{RES}/compare_128.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["workload", "family", "qubit", "K", "class",
                    "diamond_l3_cyc", "traphs_cyc", "flexgus_cyc", "flexop_cyc", "tpu_cyc",
                    "spdup_vs_hs", "spdup_vs_flexgus", "spdup_vs_tpu",
                    "traphs_status", "flexgus_status", "flexop_status"])
        for (wl, q) in sorted(d, key=lambda k: (fam(k[0]), int(k[1]))):
            K, cls = meta[(wl, q)]
            g = d[(wl, q)]
            dia = num(g.get("diamond_l3", ("", ""))[0])
            hs = num(g.get("traphs", ("", ""))[0]);   hs_s = g.get("traphs", ("", "-"))[1]
            fg = num(g.get("flexgus", ("", ""))[0]);  fg_s = g.get("flexgus", ("", "-"))[1]
            fo = num(g.get("flexop", ("", ""))[0]);   fo_s = g.get("flexop", ("", "-"))[1]
            tp = num(tpu.get((wl, q)))
            sp = lambda x: f"{x/dia:.2f}" if (x and dia) else "-"
            w.writerow([wl, fam(wl), q, K, cls,
                        dia or "", hs or "", fg or "", fo or "", int(tp) if tp else "",
                        sp(hs), sp(fg), sp(tp), hs_s, fg_s, fo_s])

    # ---- ablation_128.csv (q14 L1/L2/L3) ----
    with open(f"{RES}/ablation_128.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["workload", "qubit", "K", "class", "L1_cyc", "L2_cyc", "L3_cyc"])
        for (wl, q) in sorted(d, key=lambda k: (fam(k[0]), int(k[1]))):
            if q != "14":
                continue
            g = d[(wl, q)]
            if not any(m in g for m in ("diamond_l1", "diamond_l2", "diamond_l3")):
                continue
            K, cls = meta[(wl, q)]
            get = lambda m: (g.get(m, ("", ""))[0] or g.get(m, ("", "X"))[1])
            w.writerow([wl, q, K, cls, get("diamond_l1"), get("diamond_l2"), get("diamond_l3")])

    n = len(d)
    print(f"merged {n} (workload,q) cells -> compare_128.csv, ablation_128.csv")
    for (wl, q) in sorted(d, key=lambda k: (fam(k[0]), int(k[1]))):
        g = {m: v[0] for m, v in d[(wl, q)].items()}
        print(f"  {wl:<16} q{q}: {g}")


if __name__ == "__main__":
    main()
