# DIAMOND — Artifact Appendix

## Artifact Check-List (Meta-Information)

- **Algorithm:** DIAMOND — offset-space diagonal (DIA) convolution dataflow for sparse matrix-power / Hamiltonian Taylor-series exponentiation on a 2-D PE mesh, with zero-skip and far-diagonal load balancing (cbalance).
- **Program:** `diamond` — cycle-accurate C++ simulator (plus `accel_compare` for the Trapezoid-HS baseline and `HBMHamiltonianAnalytic` for the analytic TPU model).
- **Compilation:** GNU Make, GCC 12 (C++17). Ramulator 2 (SOTA HBM4) is modeled in-loop; the Ramulator adapter TU is built as C++20 and linked against `external/ramulator2/libramulator.so`.
- **Binary:** `outputs/diamond`, compiled from source.
- **Data set:** HamLib Hamiltonians converted to DIA `.txt`. 8 families across all four HamLib problem domains, qubit sizes q = 14–20, built reproducibly from `isca/hamlib_provenance.csv`.
- **Hardware:** Commodity server or cluster node — ≥4 CPU cores. Memory-bound for base-level fill-in matrices (up to ~115–185 GiB per process at q18–20).
- **Run-time environment:** Linux; Python ≥ 3.8 with numpy, scipy, h5py, qiskit (dataset build) and qutip (correctness validation).
- **Metrics:** Simulated HBM-inclusive cycle counts (`max(compute, exposed-DRAM)`) and speedup relative to TPU, Flexagon, Trapezoid-HS, and SegFold.
- **Output:** Per-cell CSV rows (`isca/rows16/`), the merged comparison table (`isca/compare_16x16.csv`), and figures.
- **Experiments:** 16×16 sweep over the HamLib manifest (~185 method×workload cells) driven by SLURM array jobs.
- **How much disk space required?** ~2–20 GB (DIA data is dominated by large-q dense diagonals).
- **How much time is needed to prepare the workflow?** ~5 minutes to build the simulator; dataset build time depends on q (minutes for q ≤ 16, longer for q18–20).
- **How much time is needed to complete experiments?** Sweep runtime scales with the number of cluster nodes; per-cell runtime ranges from seconds (small q) to hours (q18–20 fill-in).
- **Publicly available?** TODO — supply the DIAMOND repository URL.
- **Code licenses?** TODO — supply the license.
- **Archived?** TODO — supply the Zenodo DOI.

## Description

### How to Access

The artifact is hosted at TODO (repository URL) and archived at TODO (Zenodo DOI).

### Hardware Dependencies

- **CPU:** 4 cores minimum.
- **RAM:** ≥ 64 GB recommended. Base-level (no zero-skip) fill-in and banded matrices at q18–20 densify under repeated squaring and can require > 115 GB per process; on the evaluation cluster each task uses a whole-node reservation with a `ulimit -v` cap (115–185 GiB). Note: this cluster reports no per-job memory tracking, so `--exclusive` is required to make the cap meaningful.
- **Disk:** 2–20 GB (source, DIA data, and outputs).
- **Toolchain:** GCC 12 (C++17; the Ramulator adapter needs C++20), GNU Make. Ramulator 2 (`external/ramulator2/libramulator.so`) provides the in-loop HBM4 DRAM model.
- **Python ≥ 3.8** with numpy, scipy, h5py, qiskit (dataset construction) and qutip (correctness validation).

### Data Sets

The experiments use Hamiltonians from the HamLib collection (`https://portal.nersc.gov/cfs/m888/dcamps/hamlib/`), converted to the offset-space DIA text format the simulator consumes. Provenance for every matrix — source hdf5 file, instance key, qubit count, matrix dimension N, nonzero-diagonal count D, nnz, sparsity, diagonal sparsity, and a key-confidence marker — is recorded in `isca/hamlib_provenance.csv`. The dataset is rebuilt reproducibly from that file:

```bash
python3 isca/fetch_from_provenance.py            # resolve hdf5 + build all DIA matrices
python3 isca/fetch_from_provenance.py --verify   # build and check N/D/nnz against provenance
```

Coverage: all **11 HamLib Hamiltonian families** across the four problem domains, at qubit sizes q = 14–20 — condensed matter (tfim, heisenberg, fermihubbard, bosehubbard), binary optimization (maxcut, qmaxcut, max3sat), chemistry (electronic, vibrational), and discrete optimization (tsp, maxkcut). Note: HamLib's `lattices` entry is graph-substrate metadata (edge lists, not Hamiltonians) and is therefore not a coverable Hamiltonian family; the vibrational and chemistry-electronic families are fill-in and become infeasible to assemble at the largest sizes (e.g. `O2_20`, `vib_20` are dropped), consistent with the base-dataflow OOM behavior the paper analyzes.

## Installation

Native build.

1. Build Ramulator 2 (once) so `external/ramulator2/libramulator.so` exists (see `RAMULATOR_SETUP.md`).
2. Build the simulator:

```bash
make
```

This produces `outputs/diamond` (and `outputs/accel_compare`, `outputs/HBMHamiltonianAnalytic` for the baselines).

## Experiment Workflow

Reproduce the full 16×16 comparison via the SLURM sweeps, then merge and combine:

```bash
sbatch --array=0-N%10 isca/sweep_16x16.sbatch        # DIAMOND (L1/L2/L3), TPU, Trapezoid-HS
sbatch --array=0-M%10 isca/sweep_16x16_segfold.sbatch # SegFold baseline (HBM4)
python3 isca/merge_16x16.py                           # per-method CSVs (dedup: prefer OK)
python3 isca/combine_16x16.py                         # unified compare_16x16.csv (all baselines)
```

A single matrix can be evaluated directly:

```bash
./outputs/diamond -row=16 -col=16 -folder=dia_families/ -file=qmaxcut_14_4.txt \
    -qubit=14 -iter=4 -zeroskip=1 -cbalance=1 -csv=out.csv
```

## Evaluation and Expected Results

The simulator is deterministic: for a fixed matrix and configuration the reported cycle count is reproducible. All baselines are reported on one **HBM-inclusive** basis (`max(compute, exposed-DRAM)` at 1 GHz) so the comparison is fair: DIAMOND and TPU fold in the analytic HBM number via `-csv`; Trapezoid-HS, Flexagon, and SegFold already report full-system cycles. `isca/combine_16x16.py` emits `isca/compare_16x16.csv` with one row per workload and a cycle column per method plus DIAMOND-relative speedups.

## Experiment Customization

- Dataflow levers on `diamond`: `-zeroskip` (skip structural zeros), `-cbalance` (far-diagonal balancing), `-hermitian` (stream offsets ≥ 0), `-iter=K` (Taylor order), `-reduce_lanes=W` (reduction-network width).
- Array geometry: `-row`, `-col` (e.g. 16×16 = 256 PEs).
- DRAM model: `config/hbm4_sota.yaml` (Ramulator 2 HBM4).
- The standalone `conv_breakdown` driver reports the per-part cycle breakdown (compute vs cbalance gather vs fill/drain) without touching the production binary.
