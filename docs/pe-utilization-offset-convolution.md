---
title: Improving PE Utilization — Offset-Space Convolution Dataflow
date: 2026-07-02
tags: [sparseTensorSimulator, dataflow, pe-utilization, diagonal-matmul, optimization]
status: proposed
---

# Improving PE Utilization — Offset-Space Convolution Dataflow

## Context

The accelerator computes $H^k$ (repeated sparse diagonal matmuls) to evaluate a truncated Taylor series for $e^{-iHt}\approx\sum_{j=0}^{k}\frac{(-iHt)^j}{j!}$, where $k$ is the Taylor convergence order. Each power $H^{j}=H^{j-1}\!\cdot\!H$ is one sparse matmul on the PE mesh, so the array runs $k-1$ times per evolution and any per-matmul inefficiency compounds.

The current PE performs an **index-comparison merge-join**: A-diagonals stream down columns, B-diagonals stream right along rows, and PE$(i,j)$ merges A-diagonal $j$ with B-diagonal $i$, emitting a product when $i_A+d_A=i_B$ (the comparator table). Mismatches advance/forward the smaller-index operand.

## The problem: MAC utilization collapses on many-diagonal Hamiltonians

Measured on the validated Python mesh model (`tools/dataflow_ablation/util2.py`, 2-phase 1-hop/cycle, results checked against dense $A@B$):

| Matrix | Diagonals | PEs | Cycles | Busiest PE (MACs) | MAC utilization |
|--------|-----------|-----|--------|-------------------|-----------------|
| tridiag n=64 | 3 | 9 | 71 | 64 | 89% |
| penta n=64 | 5 | 25 | 84 | 64 | 74% |
| heis n=64 | 13 | 169 | 762 | 64 | 6.1% |
| heis n=128 | 15 | 225 | 1758 | 128 | 5.5% |

> [!warning] ~6% MAC utilization for Heisenberg-class matrices
> The busiest PE does only 64 MACs, but the makespan is 762 cycles — a ~12x inflation. It is not load imbalance (nearly all PEs are active); it is offset-skew and pass-through: to align diagonals whose offsets span $\pm n/2$, the merge-join burns ~offset cycles forwarding non-matching operands instead of multiplying. For $H^k$ this penalty is paid $k-1$ times and grows as later powers fill in more (and wider) diagonals.

## The idea: matching is a deterministic shift, so use offset-space convolution

In diagonal storage the contraction index is fixed by the offsets: element $k$ of A-diagonal $d_A$ (row $k$, col $k+d_A$) matches element $k+d_A$ of B-diagonal $d_B$, and their product lands on output diagonal $d_A+d_B$. So there is nothing to *search* — the matmul is a convolution over diagonal offsets:

$$C_{\text{diag}}[d_C] \;=\; \sum_{d_A+d_B=d_C} A_{\text{diag}}[d_A]\;\odot\;\mathrm{shift}\!\big(B_{\text{diag}}[d_B],\,d_A\big)$$

Each PE handles one $(d_A,d_B)$ pair and streams the aligned overlap doing one useful MAC per cycle — no comparator, no mismatch/forward cycles. The reduction accumulates products into the $d_C=d_A+d_B$ bin. Utilization loss drops to just pipeline fill/drain plus short-diagonal imbalance.

## Tested result

`tools/dataflow_ablation/conv_idea.py` computes $A@B$ both ways and checks against dense (all correct):

| Matrix | Merge-join cycles | Merge MAC-util | Conv cycles | Conv MAC-util | Speedup |
|--------|-------------------|----------------|-------------|---------------|---------|
| heis n=32 | 326 | 6.8% | 54 | 41.6% | 6.0x |
| heis n=64 | 762 | 6.1% | 90 | 52.5% | 8.5x |
| heis n=128 | 1758 | 5.5% | 158 | 62.2% | 11.1x |

The speedup grows with problem size, and the result is bit-correct against `numpy` $A@B$.

## Implementation (done) and what actually changes

Implemented in `main/HBMHamiltonian.cpp` behind flags:
`-dataflow=merge|conv|convgrid`, `-zeroskip`, `-cbalance` (far-diagonal balancing), `-fused` (Hermitian `-hermitian` also exists but is excluded from the sweep ladder).

- `conv` — fast analytic model (`convMatmul`): computes $C$ via offset-space convolution and reports the analytic makespan. Scales to large $q$; used by the sweep.
- `convgrid` — **cycle-accurate on the real `PE` array** (`convOnGrid`): each $(d_A,d_B)$ pair is one `PE` fed its **offset-aligned** operand pair, so the *unchanged* merge-join `PE::cycle()` does a MAC + advance-both every cycle. Small-$q$ validator.

> [!important] It is a data-mapping change, not new hardware
> Same PE array, same PE compute (comparator + MAC, reused verbatim), and the **same diagonal-accumulation reduction** ($d_C=d_A+d_B$, sum same-position products). The only change is *how operands are mapped/fed*: the edge scratchpad delivers each PE its pre-aligned pair instead of streaming raw diagonals that the comparator must search. So the win is a scheduling/mapping optimization on the existing systolic array and reduction datapath.

Validated with `-verify` at small $q$ (bit-exact vs dense), and `convgrid` (real PEs) reproduces the analytic `conv` cycle count exactly — e.g. `ham_JW-8` iter=3: merge 34185 → conv/convgrid **768** cyc (44x); `heis` Lx-10 iter=2: merge 36554 → convgrid **2048** cyc (17.8x). Both PASS.

## Sweep as a gradual ablation ladder

`isca/dia_sweep.sh` sweeps the whole `dia_oom` library with a ladder that adds one improvement at a time (conv is the final dataflow; merge and Hermitian are excluded), with $k$ = the per-matrix Taylor order from the filename:

| Level | flags | adds |
|-------|-------|------|
| L1 | `conv` | offset-space convolution |
| L2 | `conv,zeroskip` | skip structural zeros in diagonals |
| L3 | `conv,zeroskip,cbalance` | far-diagonal balancing |
| L4 (final) | `conv,zeroskip,cbalance,fused` | fused Taylor accumulation |

CSV columns record every axis (`dataflow,zeroskip,cbalance,hermitian,fused`) so each level's incremental gain is directly comparable across the library.

## Ablation: convolution + zero-skip + far-diagonal balancing

Three complementary improvements, each removing a different waste, tested as an ablation on the validated model (`tools/dataflow_ablation/ablation.py`, all configs checked bit-exact against dense $A@B$):

- **conv** — offset-space convolution (aligned MAC, no index search); removes the offset-skew makespan inflation.
- **+zero-skip** — a PE spends a cycle only on a *nonzero* product; removes MACs on the structural zeros stored inside diagonals (real `heis_18` is ~73% intra-diagonal zeros).
- **+balance** — segment the long (near) diagonals across the idle far-diagonal PEs; fills the leading/trailing idle of short far-diagonal pairs.

Makespan in cycles (speedup vs the merge-join baseline in parentheses):

| Case | useful MACs | merge | conv | +zero-skip | +balance | +both |
|------|-------------|-------|------|-----------|----------|-------|
| heis dense n=64 | 6546 | 358 | 64 (5.6x) | 64 (5.6x) | 55 (6.5x) | 55 (6.5x) |
| heis 30% n=64 | 548 | 154 | 64 (2.4x) | 14 (11x) | 55 (2.8x) | 5 (30.8x) |
| heis dense n=128 | 18710 | 826 | 128 (6.5x) | 128 (6.5x) | 111 (7.4x) | 111 (7.4x) |
| heis 30% n=128 | 1573 | 264 | 128 (2.1x) | 40 (6.6x) | 111 (2.4x) | 10 (26.4x) |

PE MAC-utilization rises from **3–15% (merge-join) to 91–99.7% (conv+zero-skip+balance)**.

> [!note] The levers are complementary, not redundant
> Zero-skip does nothing on dense diagonals (nnz = band length) but is decisive on realistic sparse diagonals; balancing is modest alone (~1.2x) but essential *after* zero-skip, which makes pair lengths very uneven. Combined they give up to ~30x fewer compute cycles on realistic sparse data.

## Full ablation over a Taylor sequence (compute + memory levers)

Evaluating $S=\sum_{j=0}^{k} H^j/j!$ (as $P_1=H,\ P_j=P_{j-1}H$, accumulating) exercises all five levers end-to-end. Verified bit-exact against the dense Taylor sum (`tools/dataflow_ablation/full_ablation.py`); HBM at 2.048 TB/s, accel 1 GHz, double-buffered overlap. heis n=64, Hermitian, k=5:

| Config | compute cyc | HBM MB | compute us | mem us | e2e us | mem% |
|--------|-------------|--------|-----------|--------|--------|------|
| dense — merge baseline | 4096 | 0.48 | 4.10 | 0.24 | 4.10 | 5% |
| conv+zeroskip+balance | 183 | 0.48 | 0.18 | 0.24 | 0.24 | 56% |
| +hermitian | 183 | 0.25 | 0.18 | 0.12 | 0.18 | 40% |
| +hermitian+fused | 183 | 0.04 | 0.18 | 0.02 | 0.18 | 9% |
| sparse 30% — merge baseline | 3036 | 0.37 | 3.04 | 0.18 | 3.04 | 6% |
| sparse 30% — all five levers | 62 | 0.03 | 0.06 | 0.02 | 0.06 | 21% |

End-to-end: **22x (dense), 49x (sparse)**. The levers interlock: the merge baseline is compute-bound; the compute levers make it memory-bound; then Hermitian symmetry (H and every $H^j$ are Hermitian, so store offsets $\ge 0$ only) halves traffic and fused accumulation collapses it ~12x (never write/re-read intermediate powers to HBM).

> [!note] Memory levers
> - **Hermitian symmetry**: ~2x less operand/result traffic and storage.
> - **Fused Taylor accumulation**: keep the running sum $S$ and current power $P_j$ on-chip, stream $H$ once — removes the per-power HBM round-trip that dominates the current design's traffic (the C-spill hit 150 MB / ~1.1 GB written at q=16). **Bounded by the 2 MB scratchpad (ctile+fused)**: the part of the on-chip footprint that exceeds the budget spills to HBM (written out, read back), so `fused` degrades gracefully instead of assuming everything fits. The reported `spm_peak_kib` / spill columns show when it spills.
>
> Note: computing $H^k$ can densify (a wide Hamiltonian's powers fill in many diagonals), so `convMatmul` is sparse (O(nnz), not O(#offsets·n)) to avoid OOM; very high $k$ on wide-offset matrices is still fundamentally limited by the fill-in of the exact power.

## Bottleneck reality check (memory-bound)

These are **compute** improvements. The sim's own numbers show the workload is **memory-bound**: heis q=16 reports mem-latency = 82.7% (compute ~18% of runtime). So an N-fold compute speedup nets only ~1/(1-0.83) ≈ 5x end-to-end at best unless HBM traffic also drops. The convolution/zero-skip/balancing wins are primarily **efficiency** (area/energy, PE utilization); the biggest **runtime** levers are memory-side: Hermitian symmetry (~2x traffic), fused Taylor accumulation (avoid materializing each $H^j$ to HBM), and reducing C spill.

## Caveats

- The ablation makespans are from an **analytic model** (conv = longest pair length; zero-skip = nonzero-product count; balance = total-work / PEs). They are optimistic upper bounds: real hardware adds pipeline fill/drain, control, and imperfect segmentation, so expect somewhat less than the headline speedups.
- Zero-skip needs per-diagonal nonzero indexing (a compressed/bitmap operand stream) so a PE can advance past stored zeros in O(1); balancing needs a scheduler that segments long diagonals across PEs.
- This is a genuine dataflow/PE redesign, not a config flag; it should land behind a mode switch (e.g. `-dataflow=conv[,zeroskip,balance]`) and be `-verify`-checked before it feeds any reported result.
- Reproduce: `tools/dataflow_ablation/ablation.py` (makespan + utilization, verified) and `tools/dataflow_ablation/conv_idea.py` (convolution vs merge-join).

## Related

- [[2026-07-02-change-summary]] — per-PE FIFO vs edge scratchpad; the O(n) edge-buffer requirement.
- The FIFO analysis (`tools/dataflow_ablation/dataflow_check.py`) established that alignment buffering is the scratchpad's job; this idea removes most of the *cycles* that buffering was covering for.
