#!/usr/bin/env python3
"""Overlap cycle-breakdown figure for the offset-space DIA convolution (16x16 grid).

Shows how much of the cbalance flexible-NoC gather/routing cost is HIDDEN behind
compute (covered by the scatter->MAC->gather pipeline overlap) vs EXPOSED in the
makespan. Drives the standalone `conv_breakdown` binary (does NOT touch diamond),
assembles result/overlap_breakdown_16.csv, and renders serial-vs-pipelined bars.

Per workload the model reports (summed over the K powers), from convOnGrid:
  compute_mac   busiest-PE MAC makespan
  gather_thr    TOTAL cbalance NoC/reduce cost   (hidden = min(compute,gather))
  filldrain     one-time pipeline ramp = 2(S-1)+ceil(log2 P)
  makespan  = compute_mac + max(0,gather_thr-compute_mac) + filldrain
  serial    = compute_mac + gather_thr + filldrain      (hypothetical no-overlap)
  overlap_savings = serial - makespan = hidden gather cycles

Usage (compute node, hamlib mounted):
  python3 isca/overlap_breakdown_16.py --grid 16 \
      --ham /mnt/beegfs/ysu34/hamlib --reduce-lanes 0

Local smoke render (no hamlib):
  python3 isca/overlap_breakdown_16.py --files scratch_asym/asym_6.txt \
      --grid 16 --iter 2 --reduce-lanes 0,8

matplotlib is optional: if absent, the CSV + a text table are still produced.
"""
import argparse, csv, os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "outputs", "conv_breakdown")


def strip_txt(s):   # str.removesuffix is 3.9+; this node has 3.6
    return s[:-4] if s.endswith(".txt") else s

# The q14 families used by the end-to-end figure (end_to_end_128.py:FAMILIES).
# (folder, file, K). Reused here so the breakdown matches the same workloads.
FAMILIES = [
    ("dia_oom",      "tfim_14_4.txt",     4),
    ("dia_families", "reg3_14_4.txt",     4),
    ("dia_families", "maxcut_14_5.txt",   5),
    ("dia_families", "qmaxcut_14_4.txt",  4),
    ("dia_families", "fermi_14_4.txt",    4),
    ("dia_oom",      "sweepheis_14_5.txt", 5),
    ("dia_families", "chem_14_4.txt",     4),
]

COLS = ["tag", "file", "n", "S", "K", "zeroskip", "cbalance", "reduce_lanes",
        "compute_mac", "gather_thr", "filldrain", "gather_exposed", "gather_hidden",
        "makespan", "serial", "overlap_savings"]
INT_COLS = COLS[2:]   # everything after file is integer


def run_case(dia_path, tag, grid, it, zeroskip, cbalance, rl, out_csv):
    """Invoke conv_breakdown for one (matrix, reduce_lanes); appends a row to out_csv."""
    cmd = [BIN, f"-file={dia_path}", f"-row={grid}", f"-col={grid}",
           f"-zeroskip={zeroskip}", f"-cbalance={cbalance}", f"-reduce_lanes={rl}",
           f"-tag={tag}", f"-csv={out_csv}"]
    if it:
        cmd.append(f"-iter={it}")
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)   # 3.6-safe (no capture_output/text)
    if r.returncode != 0:
        sys.stderr.write(f"  FAIL {tag} rl={rl}: {r.stderr.strip() or r.stdout.strip()}\n")
        return False
    sys.stderr.write(r.stdout)
    return True


def read_rows(out_csv):
    rows = []
    with open(out_csv) as f:
        for r in csv.DictReader(f):
            for c in INT_COLS:
                r[c] = int(r[c])
            rows.append(r)
    return rows


def check_identity(rows):
    """Reconciliation must hold exactly (the whole point of the breakdown)."""
    bad = 0
    for r in rows:
        lhs = r["compute_mac"] + r["gather_exposed"] + r["filldrain"]
        if lhs != r["makespan"]:
            sys.stderr.write(f"  IDENTITY VIOLATED {r['tag']} rl={r['reduce_lanes']}: "
                             f"{lhs} != makespan {r['makespan']}\n")
            bad += 1
        if r["serial"] - r["makespan"] != r["overlap_savings"]:
            sys.stderr.write(f"  SAVINGS MISMATCH {r['tag']} rl={r['reduce_lanes']}\n")
            bad += 1
    return bad == 0


def text_table(rows):
    hdr = ["tag", "rl", "compute", "gather", "hidden", "exposed", "fill",
           "makespan", "serial", "saved", "hidden%"]
    print("  ".join(f"{h:>9}" for h in hdr))
    for r in rows:
        hpct = 100.0 * r["gather_hidden"] / r["gather_thr"] if r["gather_thr"] else 0.0
        vals = [r["tag"], r["reduce_lanes"], r["compute_mac"], r["gather_thr"],
                r["gather_hidden"], r["gather_exposed"], r["filldrain"],
                r["makespan"], r["serial"], r["overlap_savings"], f"{hpct:.0f}"]
        print("  ".join(f"{str(v):>9}" for v in vals))


def plot(rows, out_fig, rl_focus):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ImportError:
        sys.stderr.write("matplotlib not available -> skipping figure (CSV + table still written)\n")
        return False

    # One bar-pair per workload at the focus network width (default full-width rl=0).
    sub = [r for r in rows if r["reduce_lanes"] == rl_focus]
    if not sub:
        sys.stderr.write(f"no rows at reduce_lanes={rl_focus} to plot\n")
        return False
    sub.sort(key=lambda r: r["tag"])
    labels = [r["tag"] for r in sub]
    x = range(len(sub))
    w = 0.38
    # Colorblind-safe (Okabe-Ito): compute=blue, gather/exposed=orange, filldrain=gray.
    C_COMPUTE, C_GATHER, C_FILL = "#0072B2", "#E69F00", "#999999"

    fig, ax = plt.subplots(figsize=(max(6, 1.1 * len(sub)), 4.2))
    for i, r in enumerate(sub):
        # Serial (no overlap): compute | gather_thr | filldrain
        ax.bar(i - w/2, r["compute_mac"], w, color=C_COMPUTE)
        ax.bar(i - w/2, r["gather_thr"], w, bottom=r["compute_mac"], color=C_GATHER)
        ax.bar(i - w/2, r["filldrain"], w, bottom=r["compute_mac"] + r["gather_thr"], color=C_FILL)
        # Pipelined (actual makespan): compute | exposed gather | filldrain
        ax.bar(i + w/2, r["compute_mac"], w, color=C_COMPUTE)
        ax.bar(i + w/2, r["gather_exposed"], w, bottom=r["compute_mac"], color=C_GATHER, hatch="///")
        ax.bar(i + w/2, r["filldrain"], w, bottom=r["compute_mac"] + r["gather_exposed"], color=C_FILL)
        # Annotate the covered (hidden) routing = the gap between the two bars.
        if r["overlap_savings"] > 0:
            top = r["serial"]
            ax.annotate(f"−{r['overlap_savings']}\nhidden", (i, top), ha="center", va="bottom",
                        fontsize=8, color=C_GATHER)

    ax.set_xticks(list(x)); ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.set_ylabel("cycles")
    ax.set_title(f"cbalance routing: serial vs pipelined (16×16 grid, reduce_lanes={rl_focus})\n"
                 "left=serial (Σ stages)  right=pipelined (makespan)")
    legend = [Patch(color=C_COMPUTE, label="compute (MAC)"),
              Patch(color=C_GATHER, label="gather / routing"),
              Patch(facecolor=C_GATHER, hatch="///", label="gather EXPOSED"),
              Patch(color=C_FILL, label="fill/drain")]
    ax.legend(handles=legend, fontsize=8, frameon=False, ncol=2)
    fig.tight_layout()
    fig.savefig(out_fig)
    fig.savefig(out_fig.replace(".pdf", ".png"), dpi=150)
    print(f"wrote {out_fig}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ham", default="/mnt/beegfs/ysu34/hamlib",
                    help="hamlib root (folder/<file> from FAMILIES)")
    ap.add_argument("--files", default="", help="comma list of DIA paths (overrides FAMILIES)")
    ap.add_argument("--grid", type=int, default=16)
    ap.add_argument("--iter", type=int, default=0, help="0 => derive K from filename")
    ap.add_argument("--zeroskip", type=int, default=1)
    ap.add_argument("--cbalance", type=int, default=1)
    ap.add_argument("--reduce-lanes", default="0",
                    help="comma list; 0=full P-wide network. e.g. 0,64,16")
    ap.add_argument("--out-csv", default=os.path.join(REPO, "result", "overlap_breakdown_16.csv"))
    ap.add_argument("--out-fig", default=os.path.join(REPO, "result", "overlap_breakdown_16.pdf"))
    ap.add_argument("--plot-rl", type=int, default=0, help="network width to render (default full)")
    a = ap.parse_args()

    if not os.path.exists(BIN):
        sys.exit(f"missing {BIN} -- run `make conv_breakdown` first")
    os.makedirs(os.path.dirname(a.out_csv), exist_ok=True)
    if os.path.exists(a.out_csv):
        os.remove(a.out_csv)   # raw run output; regenerate cleanly each time

    rl_list = [int(x) for x in a.reduce_lanes.split(",") if x != ""]

    # Build the (dia_path, tag, K) worklist.
    work = []
    if a.files:
        for p in a.files.split(","):
            work.append((p, strip_txt(os.path.basename(p)), a.iter))
    else:
        for folder, fil, K in FAMILIES:
            p = os.path.join(a.ham, folder, fil)
            if not os.path.exists(p):
                sys.stderr.write(f"  MISSING {p} (skipped)\n"); continue
            work.append((p, strip_txt(fil), a.iter))
    if not work:
        sys.exit("no reachable input matrices -- pass --files or mount hamlib")

    for dia_path, tag, K in work:
        for rl in rl_list:
            run_case(dia_path, tag, a.grid, K, a.zeroskip, a.cbalance, rl, a.out_csv)

    rows = read_rows(a.out_csv)
    print(f"\n== breakdown ({len(rows)} rows) -> {a.out_csv} ==")
    text_table(rows)
    ok = check_identity(rows)
    print("identity:", "OK" if ok else "FAILED")
    plot(rows, a.out_fig, a.plot_rl)


if __name__ == "__main__":
    main()
