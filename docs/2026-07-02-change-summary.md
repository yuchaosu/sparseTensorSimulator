---
title: HBM Accelerator Simulator — Change Summary
date: 2026-07-02
branch: cleanup-and-hbm-sources
tags: [sparseTensorSimulator, hbm, ramulator, ablation, changelog]
---

# HBM Accelerator Simulator — Change Summary (2026-07-02)

Branch `cleanup-and-hbm-sources`, 12 commits, not yet pushed. The work took the simulator from a mid-cleanup state to a single consolidated tree with a Ramulator-backed SOTA HBM4 model, an edge-scratchpad accelerator model, a ground-truth verification harness that found and fixed a real datapath bug, a measured tiling ablation, and a multi-node SLURM sweep.

## Repo cleanup and consolidation

- Removed build artifacts and scattered run logs; fixed `.gitignore` (including a trailing-space bug that had disabled `*.png`); tracked the previously-untracked HBM / verilog / tools sources.
- Merged `Wzh/` into the repo root so there is a single canonical tree; the older duplicate copy is gone.

## HBM = Ramulator 2.1 (SOTA HBM4), in-loop

- Added `RamulatorHBM` — a C++17 PIMPL adapter (compiled C++20, links `libramulator.so`) that is a drop-in for the old homegrown DRAM engine. Config `config/hbm4_sota.yaml` models one SOTA HBM4 stack: 32 channels, 64 GB, 2.048 TB/s.
- Reports the **memory-latency percentage** of the whole process and HBM time in both ns and DRAM cycles. This also removed the old impossible-bandwidth bug (e.g. 106889 GiB/s became a sane ~117 GiB/s).

## Edge-scratchpad accelerator model (2 MB)

- On-chip C accumulation (about 100x less HBM traffic than the per-tile round-trip), operand reuse (B resident, A loaded once per row), double-buffered A prefetch overlap, and C bounded to the budget with write-back spill (bulk spill modeled bandwidth-bound so it stays fast).

## Correctness: verification harness and two real fixes

- Added `-verify` — a dense ground-truth matrix-power check (q<=12) that immediately caught silent datapath bugs.
- **C1**: the PE now stalls on an empty input instead of dropping the present operand; the injection-finished signal is latched and propagated; boundary PEs drain their pass-through operand.
- **C3**: `buildDatapackage` now sorts each diagonal stream by row. Intermediate results were stored in arrival (unsorted) order, so multi-tile runs of general matrices silently dropped merge matches from H^3 onward.

> [!important] The C3 bug was invisible without `-verify`
> Single-tile runs, and heis (power-of-2 offsets), passed by luck; general matrices (e.g. B2 chemistry) were silently wrong. After the fix the minimal repro, B2 (grid=64), and heis (grid=32) all verify exactly. A broad families x grids verification was still finishing in the background at write time.

## Tiling ablation (measured on Heis Lx-10, iter=2)

Ablation flags `-reuse` (#1), `-ctile` (#2 C-budget), `-balance` (#3) were added; the default is the best-measured config (reuse on, C-budget on, contiguous grouping).

| Config | Compute cycles | HBM time (ns) | HBM traffic | Memory-latency % |
|--------|----------------|---------------|-------------|------------------|
| baseline (no opts) | 42,767 | 104,214 | 11.5 MiB | 65.9% |
| +#1 reuse | 42,767 | 95,494 | 10.7 MiB | 65.9% |
| +#2 C-budget/spill | 42,767 | 118,473 | 39.4 MiB | 69.3% |
| +#1+#2 (default) | 42,767 | 109,753 | 38.6 MiB | 69.3% |
| +#1+#2+#3 balanced | 34,905 | 579,354 | 958 MiB | 94.2% |

- **#1 reuse** cuts operand traffic (about -7% here, up to ~1.9x with more tiles).
- **#2 C-budget** is a fidelity constraint (adds spill to honor a finite 2 MB SPM), not a speedup.
- **#3 nnz-balanced grouping** is a regression on memory-bound workloads (-18% cycles but 25x traffic from lost C locality); now default-off, opt-in via `-balance=1`.

## Multi-node SLURM sweep (`isca/`)

Three scripts run one job per array task, spread across nodes, appending a CSV row each and merging at the end.

| File | Role |
|------|------|
| `submit_sweep.sh` | Login node: build, scan beegfs, infer qubit per matrix, write `manifest.txt`, submit the array plus a dependent merge job. |
| `sweep_array.mpi` | Job array on `partition=turin` (multi-node); each task runs one manifest line (`module load gnu12`, `LD_LIBRARY_PATH` to `libramulator.so`, per-run timeout). |
| `merge_sweep.mpi` | Runs after the array; concatenates per-task rows into `isca/sweep_results.csv`. |

Launch from the `isca/` directory:

```bash
cd isca && bash submit_sweep.sh
```

Covers all 35 HamLib matrices x grids {64,128,256} (q<=12) or {64} (q>=13), plus the tiling ablation on a q<=10 subset, with `-verify` for q<=12. Safe at any grid after the C3 fix. CSV columns: `matrix,qubit,row,col,iter,reuse,ctile,balance,compute_cycles,hbm_time_ns,hbm_dram_cycles,bytes_read,bytes_written,mem_latency_pct,spm_peak_kib,spill_reads,spill_writes,verify`.

## Non-driver review fixes

- `include/Fifo.h`: throw on overflow instead of silently dropping.
- `tools/roofline.py`: removed unjustified `oi*1.5` / `perf*0.7` fudge factors; error on the `time_s=1.0` placeholder; fixed the log-axis lower bound.
- `helper/energyCal.py`: fixed the receive-energy double-count.

## Notes and follow-ups

- Only `HBMHamiltonian` got the full treatment; `HBMHamiltonianScheduled` / `Prefetch` and the legacy `matrixMulti*` drivers were left as-is per scoping (they still compile and benefit from the shared PE dataflow fix).
- `external/ramulator2` is gitignored; `RAMULATOR_SETUP.md` documents the pinned build.
- Possible next step: a small aggregator in `tools/` that turns `sweep_results.csv` into per-workload and ablation summary tables.
