---
title: HBM Accelerator Simulator — Change Report (since 7dc08d2)
date: 2026-07-07
branch: cleanup-and-hbm-sources
base_commit: 7dc08d2
tags: [sparseTensorSimulator, baselines, ablation, cycle-accurate, changelog]
---

# HBM Accelerator Simulator — Change Report since `7dc08d2` (2026-07-07)

Baseline `7dc08d2` ("HBM driver: model DRAM with Ramulator 2.1 (SOTA HBM4)"). Since then: **16 commits** on `cleanup-and-hbm-sources` plus an **uncommitted working tree** (the analytical/cycle-accurate split from 2026-07-07 and sweep-harness edits). The arc: from a single-accelerator HBM4 model to a **multi-accelerator, shared-fairness comparison** (DIAMOND vs TPU vs Trapezoid), a **cycle-accurate ablation ladder** with raw op-counts for downstream (Verilog) energy, provenance for every workload, and a clean **cycle-accurate / analytic code split**.

The first ~11 commits (the Ramulator HBM4 model, edge scratchpad, `-verify` harness + the C1/C3 datapath fixes, tiling ablation, SLURM sweep, DIA large-q inputs, FIFO-vs-scratchpad analysis) are already written up in [[2026-07-02-change-summary]]; this report recaps them in one line each and details everything after.

## Commit inventory since `7dc08d2`

| Group | Commits | Covered in |
|-------|---------|-----------|
| Foundation (HBM4, scratchpad, verify+fixes, ablation, sweep, DIA) | `8f81236`…`52f07d3` (11) | 2026-07-02 doc |
| accel_compare + shared-fairness baselines | `6c84052` | this doc |
| HamLib provenance + key resolver | `0570b6e` | this doc |
| offset-conv dataflow doc + FIFO/scratchpad analysis | `1010be2` | this doc |
| cycle-accurate L2/L3 (zeroskip+cbalance) + op-counts | `3a0a9fd` | this doc |
| accel_compare CSV name-quoting fix | `576b858` | this doc |
| analytical/cycle-accurate split + sweep-harness edits | working tree (uncommitted) | this doc |

## Already summarized (foundation, see the 2026-07-02 doc)

- **Ramulator 2.1 SOTA HBM4** in-loop DRAM model + memory-latency %; killed the impossible-bandwidth bug.
- **2 MB edge scratchpad**: on-chip C accumulation (~100x less HBM traffic), operand reuse, double-buffered prefetch, budgeted spill.
- **`-verify` dense ground-truth harness** and the two real datapath fixes it exposed: **C1** (stall on empty input instead of dropping the operand) and **C3** (sort each diagonal stream by row, or multi-tile general matrices silently corrupt from H^3).
- **Tiling ablation** (`-reuse`/`-ctile`/`-balance`) with measured cycles/traffic; contiguous grouping made default.
- **Multi-node SLURM sweep** (`submit_sweep.sh` / `sweep_array.mpi` / `merge_sweep.mpi`).
- **DIA large-q inputs** (auto-detected `N <n> D <d>` header) reaching q≈18–28; O(n) C-accumulation.
- **FIFO-vs-scratchpad buffering analysis**: min deadlock-free depth = max diagonal offset ≈ n/2, so the alignment buffer must be the edge scratchpad, not the per-PE FIFO.

## `accel_compare` driver + shared-fairness baselines (`6c84052`)

New high-level driver (`main/accel_compare.cpp`, `include/Accelerators.h`, ~625 lines) that runs the **same** Taylor-power SpMSpM workload on four accelerators under **one literal fairness contract** — equal PEs = S·S, one Ramulator2 HBM4 config, one 2 MiB on-chip buffer — and prints a **measured** component breakdown (not analytical).

| Accelerator | Model | Substrate |
|-------------|-------|-----------|
| DIAMOND (ours) | diagonal merge-join | real PE mesh |
| TPU | dense weight-stationary MXU, n³ MACs | same S·S grid |
| Trapezoid-MS (ISCA'24) | inner-product + intersection walk | real mesh |
| Trapezoid-HS (ISCA'24) | Gustavson | de-idealized off-mesh routing |

- **Pipelined/overlapped cycle model**: stages are throughput-bound (steady = max of mac/buf/router/redux), per-tile fill/drain, pipelined adder-tree reduction folded per tile; DRAM added only when it is exposed past the compute pipeline (in-loop per-burst Ramulator2).
- **Instrumented datapath**: activity counters (`compare`/`router`/`buf_wr`/`buf_rd` in `PE`, `accumulate` in `DiagonalReduction`) with static getters + reset, so the breakdown is measured.
- Documented in `docs/design-note-diamond-and-baselines.md`.

> [!note] Trapezoid-MS is present in the driver but not in the family sweep
> `accel_compare` still builds all four; the production sweep selects **DIAMOND / TPU / Trapezoid-HS**. MS was dropped from the sweep because its O(n·nnz) intersection walk explodes on the larger workloads.

## HamLib provenance + key resolver (`0570b6e`)

`isca/hamlib_provenance.csv` records, for each `dia_oom` workload, its source HDF5 file + dataset key + verified fetch params (`final_time=1.2`, `num_timesteps=1000` ⇒ `dt=0.0012`, matching the sim; `method=dia`). `num_qubits / N / D / K` are read straight from the DIA headers/filenames (checked `2^qubits == N`).

- **tfim RESOLVED**: every tfim file has `D=1` (single diagonal at offset 0) ⇒ diagonal-only ⇒ transverse field `h=0`. NB this makes tfim a degenerate matmul workload.
- **heis / bh NEEDS_CONFIRM**: their field/encoding params were never logged at generation time, so keys carry placeholders rather than a guessed value (no fabrication).
- `tools/resolve_hamlib_keys.py` resolves placeholders definitively by re-running `fetchham_dia.py` over candidate keys and matching regenerated `(qubits, K, D)` back to each file — must run in the HamLib h5py+qiskit env.

## offset-conv dataflow doc + FIFO/scratchpad analysis (`1010be2`)

- `docs/pe-utilization-offset-convolution.md`: the offset-space convolution dataflow (the DIAMOND compute-efficiency mechanism) and its ablation ladder.
- Extended the 2026-07-02 doc with the per-PE-FIFO-vs-edge-scratchpad buffering analysis.
- Established the "core = design + baselines only; SLURM runners and analytic Python kept local (not synced)" boundary that the 2026-07-07 split formalizes.

## Cycle-accurate L2/L3 ablation + op-counts (`3a0a9fd`, `576b858`)

`convOnGrid` implements the ablation levers **on the real S·S PE mesh** (cycle-accurate makespan = busiest PE), replacing the earlier analytic-only lever accounting.

- **L2 `zeroskip`** (energy lever): a zero product never enters a PE queue ⇒ fewer real MAC cycles. Scales with fill-in.
- **L3 `cbalance`** (cycle lever): split long diagonals into equal contiguous chunks across all P=S·S PEs (flexible-NoC / SIGMA–Flexagon class). The base mapping keeps each output diagonal on its own reducer PE (`dC mod P`) so the longest diagonal is a **real hotspot** the lever must relieve.
- **Router** = per-flit Manhattan path length (the flexible-NoC overhead `cbalance` introduces); **accum** = one per product; **mac/compare** measured in the PE datapath. Raw counts `(mac,compare,router,accum)` emitted to the conv CSV + stdout for the downstream Verilog energy model — this run only **counts** operations.
- `accel_compare` `-csv` emits per-baseline op counts `(mac,compare,router,buf,accum,dram_ns,buffer_peak)`; OOM rows are retained for the capacity-crossover test. `576b858` quotes the accelerator name (it contains a comma) so columns don't shift.
- **L4 fused dropped** from the ladder (flag retained, unused).

> [!check] Verified at small n
> `fermi_10` PASS on all levers; `heis_10` PASS with `zeroskip` inert on its dense band and `cbalance` giving **1.63x** via hotspot relief. Larger-q ablation numbers come from the SLURM family sweep, which was still running at write time — treat those as preliminary until it completes.

## Working tree (uncommitted, 2026-07-07)

### Analytical / cycle-accurate code split

At user request ("the main folder should only contain the cycle accurate"), the analytic cost-model code was split out into a separate, git-excluded `analytical/` folder (two drivers).

- **`main/HBMHamiltonian.cpp` → cycle-accurate only** (−173/+61 net): supports `-dataflow=` `merge` (default), `convgrid` (real-mesh DIAMOND kernel), `trapezoid[_hs]`. Removed `convMatmul` (analytic makespan) and the `tpu`/`tpugrid` analytic block. A guard now rejects `-dataflow=conv|tpu|tpugrid` with `[error]` + exit 4, so a moved dataflow can never silently fall through to `merge` and be **mis-labelled** (reporting-validity fix).
- **`analytical/` (excluded via `.git/info/exclude: analytical/`)**: `HBMHamiltonianAnalytic.cpp` (verbatim pre-trim driver — still runs the analytic `conv`/`tpu` paths) + `build.sh` (reuses repo `objs/*.o` + Ramulator link → `outputs/HBMHamiltonianAnalytic`). The previously-scattered analytic Python moved here too: `accel_sweep.py`/`.mpi`/`.csv`/`.log`, `dataflow_ablation/`, `real_oom_scipy.py`, `memory-limits.md`, `rerun.sh` (internal paths repointed).

> [!important] convgrid behavior is provably unchanged by the split
> The trimmed driver was built to scratchpad (not over the live-sweep binary) and its `convgrid` CSV is **byte-identical** to the untrimmed binary across all three ablation levels (`fermi_10_4`, zs/cb = 0-0/1-0/1-1). The analytic driver runs `-dataflow=conv` (verify PASS); `merge` default intact.

### Sweep-harness edits (`isca/*.mpi`, uncommitted)

Independent of the split: `merge_sweep.mpi` / `sweep_array.mpi` / `submit_sweep.sh` gained overridable `SWEEP_MANIFEST`/`SWEEP_ROWDIR`/`SWEEP_CSV`, optional dataflow-ablation manifest fields (`df/zs/cb/herm/fu`, default `merge`), `dia_oom` large-q manifest rows (`DIA_QMAX`), an extended CSV header, and a partition switch to `turin128` for high-q OOM headroom.

## Verification status

| Check | Result |
|-------|--------|
| convgrid CSV, trimmed vs untrimmed (3 levels, fermi_10_4) | byte-identical |
| analytic driver `-dataflow=conv` (verify) | PASS |
| main/ rejects `conv`/`tpu`/`tpugrid` | exit 4, clear error |
| merge default | runs |
| L2/L3 small-case (fermi_10, heis_10) | PASS |
| family SLURM sweep (245239) | running at write time |

## Housekeeping notes / follow-ups

- **`outputs/HBMHamiltonian` intentionally NOT rebuilt** while family sweep `245239` runs (avoid a mid-sweep binary swap; behavior is identical but reproducibility hygiene). Rebuild with `make HBMHamiltonian` after it completes.
- The split (`main/HBMHamiltonian.cpp`) and the doc are **uncommitted** — no push was requested.
- The superseded, unsynced `isca/dia_sweep.sh` still emits analytic `conv` rows; those now need `outputs/HBMHamiltonianAnalytic` (would exit 4 on the cycle-accurate binary).
- **Git hygiene caveat**: 11 of the 16 commits in range (the 2026-07-02 batch, `8f81236`…`52f07d3`) carry a `Co-Authored-By: Claude …` trailer, which conflicts with the global "commits authored by me alone" rule. The 5 recent commits (`6c84052`…`576b858`) are clean. Removing the old trailers would require a history rewrite + force-push — not done here; flag only.
