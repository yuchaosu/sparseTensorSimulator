---
title: DIAMOND accelerator — current design and baseline comparison
tags: [diamond, accelerator, sparse, spmspm, baselines, design-note]
date: 2026-07-04
status: living
---

# DIAMOND — current design and baseline comparison

> [!abstract] One line
> DIAMOND is a diagonal-native sparse SpMSpM accelerator that computes Taylor powers of a quantum Hamiltonian $H^k$ on a systolic merge-join PE mesh, evaluated cycle-accurately against a dense TPU MXU and two Trapezoid (ISCA'24) dataflows under a strict same-hardware fairness contract.

## 1. What the accelerator computes

The workload is the Taylor propagator of a sparse Hamiltonian, $U = e^{-iHt} \approx \sum_{k=0}^{K} \frac{(-it)^k}{k!} H^k$. The dominant cost is forming the powers $H^2, H^3, \dots, H^K$, i.e. a chain of sparse-times-sparse matrix products (SpMSpM). $H$ is stored in **DIA (diagonal) format**: each nonzero band is one offset $o$ with a value vector, so the matrix is a set of offset-tagged fibers rather than a dense grid.

Two reporting units are used throughout:

- **single** — one SpMSpM, $H \cdot H$.
- **whole** — the full chain $H^2 \dots H^K$, with $K$ chosen **dynamically per matrix** (physics convergence order), not fixed.

> [!info] Dynamic Taylor order $K$
> $K$ = smallest $k$ with $\frac{(\lVert H\rVert_1\, \Delta t)^{k+1}}{(k+1)!} \le 10^{-9}$, using $\Delta t = 0.0012$ and $\lVert H\rVert_1 = \max_{\text{row}}\sum |H_{ij}|$ (induced 1-norm). This reproduces the real `n_taylor` observed in the workload files (e.g. heis-18 → K5, heis-20 → K6, tfim-18 → K4). It is **not** a static constant.

## 2. The DIAMOND dataflow (ours)

The core is a systolic **merge-join** mesh built from the real `Grid` / `PE` / `DiagonalReduction` classes — the same RTL-adjacent C++ model used everywhere else in this repo, not an analytical stand-in.

- $A$ fibers stream **down** the columns, $B$ fibers stream **right** across the rows.
- A PE fires a MAC only when the streamed indices meet the contraction condition ($A.\text{col} = B.\text{row} = k$); every non-matching tick is a real comparison, not a wasted MAC.
- Products drain into the per-PE `DiagonalReduction` port, which accumulates by output diagonal (offset-space), so results are produced already in DIA format for the next power.

This is the key structural advantage: the merge-join consumes the two sparse operands **in offset space**, so the machine never materializes the dense $n \times n$ intermediate and never issues a MAC for a structural zero.

> [!note] Sparsity along the diagonal
> DIA assumes a band is dense, but real Hamiltonian bands have interior zeros. The mesh still pays a *compare* for those, but **not a MAC** — the MAC count equals the true nonzero-contraction count. The offset-space accumulation reported in [[pe-utilization-convolution]] is what turns the ~6% naïve merge-join MAC utilization into the far higher effective throughput seen below.

## 3. Cycle model — pipelined and overlapped

Cycles are **not** a MAC count. Every structural stage consumes time and the stages are overlapped as a real pipeline:

- Per-stage throughput cost: `mac_cyc`, `buf_cyc = ceil(buf/(PEs·ports))`, `rout_cyc = ceil(router/(PEs·links))`, plus a **pipelined adder-tree** reduction folded per tile.
- Steady state is throughput-bound: `steady = max(mac_cyc, buf_cyc, rout_cyc, redux_cyc)`.
- Fill/drain is charged **per tile**: `+2S + ceil(log2 S)` for an $S\times S$ array.
- Memory is layered on top via the real in-loop DRAM model (§5): `real_cyc = pipeline + max(0, dram_ns − pipeline)` — DRAM only adds cycles when it is *exposed* past the compute pipeline.

The activity counters (`mac`, `compare`, `router`, `buf_wr/rd`, `accumulate`) are **measured at the datapath sites** in `PE.cpp` / `DiagonalReduction.cpp` during the real run, then fed to `computeBreakdown()`. Nothing in the breakdown is a closed-form guess.

> [!important] Energy vs. cycle percentages are different axes
> The `MEM%` column is an **energy** share (weights MAC=1, BUF=1, ROUTER=2, ACCUM=1, MEM=200 per 32 B). A workload can be 80% memory *energy* while memory contributes **zero exposed cycles** (`exposed=0`) because streaming is fully hidden behind compute. Do not read `MEM%` as "memory-bound in time."

## 4. The fairness contract (shared, literally)

Every accelerator is run through the **same three constants** — not similar, shared in code:

| Resource | Value | Shared via |
|---|---|---|
| PE count | $S^2 = 64$ ($8\times8$) | `-row=S`, `PEs = S*S` |
| Main memory | Ramulator 2.1, SOTA HBM4 | `config/hbm4_sota.yaml`, in-loop per-burst |
| On-chip buffer | 2 MiB | `kScratchpadBytes` |

`DataPackage` = 16 B for all. The TPU gets the **same real PE grid** (an $S\times S$ MXU with $S^2$ MACs), not a bespoke larger array. This is what makes the cycle deltas attributable to the *dataflow*, not to unequal silicon.

## 5. Memory model

DRAM is the real Ramulator 2.1 HBM4 model, driven **in-loop per burst** (`AccelHBM` wrapping `RamulatorHBM`), with fresh state per accelerator so no run inherits another's row-buffer. The earlier peak-bandwidth shortcut (`kHBM4PeakBytesPerNs`) was removed from the C++ path; only the analytical sweep companion (`isca/accel_sweep.py`) still uses a Ramulator-calibrated effective bandwidth, and it is labeled `source=analytical` in the CSV.

## 6. The baselines

- **TPU (dense MXU)** — weight-stationary $S\times S$ systolic array doing the *dense* product ($n^3$ MACs, $n^2$ bytes). Same PEs, same HBM, same buffer. This is the "ignore sparsity" reference.
- **Trapezoid-MS (inner-product)** — the ISCA'24 Trapezoid multi-stationary / inner-product dataflow: every output visited, each pair walking $|A_{\text{row}}| + |B_{\text{col}}|$. Intersection waste is real and measured.
- **Trapezoid-HS (Gustavson)** — the Trapezoid Gustavson / expand-merge dataflow: pre-matched products, zero wasted compares, but routing/accumulation de-idealized (3 hops + 2 buffer writes per product) since Gustavson cannot run natively on the merge-join mesh.

> [!warning] Trapezoid has no public artifact
> Trapezoid (Yang / Emer / Sanchez, MIT, ISCA'24) ships **no** code. Both dataflows here are faithful reimplementations validated against the paper's *qualitative* claims (dense-MS ≈ TPU cycle count; intersection-waste behavior). The paper's headline gmean speedups are **not** reproducible without the artifact and are not claimed here.

## 7. Representative measured result

A single cycle-accurate point (synthetic $q=8$, $n=256$, 2 iterations, $8\times8$ mesh, real mesh for all sparse dataflows, dense formula for TPU). Full family sweep (bh / heis / tfim, $q=18\text{–}28$) is produced separately by `isca/accel_sweep.py`.

| accelerator | cycles | PE-util | MAC% | MEM% | ROUTER% | BUFFER% | MACs |
|---|---|---|---|---|---|---|---|
| DIAMOND (diagonal, ours) | 4,781 | 11.45% | 1.25 | 80.98 | 7.83 | 8.69 | 35,030 |
| Trapezoid-HS (Gustavson) | 23,105 | 2.37% | 1.27 | 82.23 | 7.62 | 7.62 | 35,030 |
| Trapezoid-MS (inner-product) | 239,959 | 0.23% | 0.32 | 20.50 | 36.85 | 42.02 | 35,030 |
| TPU (dense MXU) | 7,633,117 | 6.87% | 4.36 | 8.52 | 39.20 | 43.56 | 33,554,432 |

On this point DIAMOND is ≈**4.8×** faster than Trapezoid-HS, ≈**50×** faster than Trapezoid-MS, and ≈**1600×** faster than the dense TPU — while doing the identical 35 030 useful MACs that the dense TPU inflates to 33.5 M by ignoring sparsity. These are one-workload numbers for intuition, not the headline; treat the family sweep as the reportable result.

> [!keyidea] Why DIAMOND wins
> All three sparse machines do the *same* useful MACs. DIAMOND wins on **cycles** because offset-space merge-join keeps the product stream dense with no intersection walk (beats MS) and native DIA accumulation avoids the extra routing/buffering Gustavson needs off-mesh (beats HS). The narrative is **compute/dataflow efficiency and cycles**, not a memory-capacity story — all $H^k$ are assumed to fit; pruning is reserved for the separate GPU library. See [[narrative-not-memory-but-compute]] and [[pruning-reserved-gpu-library]].

## 8. Reproduce

```bash
make HBMHamiltonian accel_compare
# single cycle-accurate comparison point (all four accelerators, shared fairness triple)
./outputs/accel_compare -file=synth_8_3.txt -qubit=8 -iter=2 -row=8
# real-family analytical/workload sweep (bh/heis/tfim, q18-28, both units, dynamic K)
python3 isca/accel_sweep.py 10 30 /mnt/beegfs/ysu34/accel_sweep.csv
```

Pinned: Ramulator 2.1, `config/hbm4_sota.yaml`, `g++ -std=c++17 -O3`, $\Delta t = 0.0012$, convergence tol $10^{-9}$, `DataPackage` = 16 B, buffer 2 MiB, $S = 8$.
