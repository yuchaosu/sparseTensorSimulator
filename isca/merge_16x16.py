#!/usr/bin/env python3
"""Collate isca/rows16/*.csv into the four final 16x16 baseline CSVs.

Mesh rows (mesh_*.csv from sweep_16x16.sbatch) schema:
  method,level,workload,q,K,class,pe,cycles,status,sec
split by method:  diamond_l1/l2/l3 -> DIAMOND ; tpu -> TPU ; traphs -> Trapezoid
Flexagon rows (flex_*.csv from sweep_16x16_flex.sbatch) are already in final schema.
"""
import glob, os, sys

ROWS = "/home/ysu34/DIAMOND/diamond/isca/rows16"
OUT = "/home/ysu34/DIAMOND/diamond/isca"


# sweepheis is synthetic (not in any HDF5); its .txt were deleted, so sweep rows for it are
# invalid (missing-file failures). Exclude from the final dataset.
EXCLUDE_WORKLOAD_PREFIX = ("sweepheis",)


def _excluded(workload):
    return any(workload.startswith(p) for p in EXCLUDE_WORKLOAD_PREFIX)


def read_rows(pattern, wl_col):
    out, dropped = [], 0
    for f in sorted(glob.glob(os.path.join(ROWS, pattern))):
        with open(f) as fh:
            for ln in fh:
                ln = ln.strip()
                if not ln:
                    continue
                parts = ln.split(",")
                if len(parts) > wl_col and _excluded(parts[wl_col]):
                    dropped += 1
                    continue
                out.append(parts)
    if dropped:
        print(f"  ({pattern}: dropped {dropped} excluded/sweepheis rows)")
    return out


def dedup_prefer_ok(rows):
    """One row per (method,workload,q,K). Reruns (sweep_16x16_oom.sbatch) write extra
    mesh_*.csv rows for cells that originally OOM'd; keep the OK result over any non-OK
    so the reran cells replace the stale OOM/TIMEOUT rows (schema: ...,status=col8)."""
    best = {}
    for r in rows:
        if len(r) < 9:
            continue
        key = (r[0], r[2], r[3], r[4])   # method, workload, q, K
        cur = best.get(key)
        if cur is None or (r[8] == "OK" and cur[8] != "OK"):
            best[key] = r
    return list(best.values())


def dedup_prefer_ok_flex(rows):
    """One row per (impl,workload,q,K) for the 11-col flex schema (status=col9). The
    turin128 fat-node reruns write extra flex_t128_*.csv rows for q14 cells that timed
    out; keep OK over any non-OK so a recovered cell replaces its stale TIMEOUT row."""
    best = {}
    for r in rows:
        if len(r) < 10:
            continue
        key = (r[0], r[1], r[2], r[3])   # impl, workload, q, K
        cur = best.get(key)
        if cur is None or (r[9] == "OK" and cur[9] != "OK"):
            best[key] = r
    return list(best.values())


def write_csv(path, header, rows):
    with open(path, "w") as w:
        w.write(header + "\n")
        for r in rows:
            w.write(",".join(r) + "\n")
    print(f"{os.path.basename(path)}: {len(rows)} rows")


def main():
    mesh = read_rows("mesh_*.csv", wl_col=2)  # method,level,workload,q,K,class,pe,cycles,status,sec
    mesh = dedup_prefer_ok(mesh)              # collapse rerun OOM->OK duplicates, prefer OK
    dia = [r for r in mesh if r[0].startswith("diamond_")]
    tpu = [r for r in mesh if r[0] == "tpu"]
    trp = [r for r in mesh if r[0] == "traphs"]

    # DIAMOND: level,workload,q,K,class,pe,cycles,status,sec   (level from col 1)
    write_csv(f"{OUT}/data_16x16_DIAMOND.csv",
              "level,workload,q,K,class,pe,cycles,status,sec",
              [[r[1]] + r[2:10] for r in dia])
    # TPU: workload,q,K,class,pe,cycles,status,sec
    write_csv(f"{OUT}/data_16x16_TPU.csv",
              "workload,q,K,class,pe,cycles,status,sec",
              [r[2:10] for r in tpu])
    # Trapezoid: dataflow,workload,q,K,class,pe,cycles,status,sec  (dataflow=level field=Gustavson)
    write_csv(f"{OUT}/data_16x16_Trapezoid.csv",
              "dataflow,workload,q,K,class,pe,cycles,status,sec",
              [[r[1]] + r[2:10] for r in trp])
    # Flexagon: impl,workload,q,K,class,pe,cycles_total,per_step_cycles,per_step_nnz,status,sec
    flex = read_rows("flex_*.csv", wl_col=1)   # impl,workload,... (incl. flex_t128_* reruns)
    flex = dedup_prefer_ok_flex(flex)          # collapse q14 TIMEOUT->OK reruns, prefer OK
    write_csv(f"{OUT}/data_16x16_flexagon.csv",
              "impl,workload,q,K,class,pe,cycles_total,per_step_cycles,per_step_nnz,status,sec",
              flex)


if __name__ == "__main__":
    main()
