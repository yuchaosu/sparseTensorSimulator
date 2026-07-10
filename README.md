# sparseTensorSimulator — DIAMOND

Cycle-accurate simulator of a **systolic PE array** that computes **sparse
diagonal SpMSpM** (`H^{k-1} · H`) to build the Taylor powers `H^1…H^K` of a
quantum Hamiltonian, with an **in-loop Ramulator 2.1 (SOTA HBM4)** memory model.
The propagator is

```
U ≈ Σ_{k=0}^{K} (-i·dt)^k / k! · H^k      (dt = 0.0012, tol = 1e-9)
```

so the K matrix-matrix products are the whole cost, and the accelerator's cycle
count *is* the end-to-end propagator-build time.

**DIAMOND** stores each `H^k` in **DIA (diagonal-offset)** form and multiplies as
an **offset-space convolution** on the S×S mesh: offset-aligned operands make the
merge-join comparator always match, so each PE MACs every cycle. Two optimisation
levers are modelled cycle-accurately:

- **zeroskip** — drop zero products → fewer MAC cycles (an **energy/MAC** lever;
  scales with fill-in).
- **cbalance** — load-balance diagonals across all PEs via a flexible NoC (a
  **cycle** lever; removes the long-diagonal hotspot).

## Build

Needs a C++17 compiler (gcc-12+) and the Ramulator 2.1 shared lib (see
`RAMULATOR_SETUP.md`).

```bash
make HBMHamiltonian accel_compare
# runtime libs (gcc-12 libstdc++ + Ramulator); required on nodes lacking the module:
export LD_LIBRARY_PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/lib64:$PWD/external/ramulator2:$LD_LIBRARY_PATH
```

## Run

```bash
./outputs/HBMHamiltonian -row=8 -col=8 -folder=dia_families/ -file=qmaxcut_14_4.txt \
    -qubit=14 -iter=4 -dataflow=convgrid -zeroskip=1 -cbalance=1 -verify=0 -csv=out.csv
```

Key flags:

| flag | meaning |
|---|---|
| `-row -col` | mesh side S (PEs = S²; sweeps use 8×8 = 64) |
| `-folder -file` | DIA workload (folder is relative to the HamLib root) |
| `-qubit -iter` | qubits q (n = 2^q) and Taylor order K |
| `-dataflow=convgrid` | cycle-accurate offset-space convolution (the headline path) |
| `-zeroskip -cbalance` | the two ablation levers (0/1) |
| `-hermitian` | stream only offsets ≥ 0 (H is Hermitian) → ~2× traffic/compute |
| `-fused` | keep running Taylor sum on-chip (cuts per-power HBM round-trips) |
| `-fifo=N` | per-PE input-FIFO depth (finite-buffer backpressure model) |
| `-reduce_lanes=N` | cbalance reduction-net width (default P) |
| `-verify` | check each `H^k` against a dense reference (small q only) |

### Ablation ladder

- **L1** `zeroskip=0 cbalance=0` — unbalanced convolution (baseline).
- **L2** `zeroskip=1 cbalance=0` — + zeroskip (energy).
- **L3** `zeroskip=1 cbalance=1` — + cbalance (cycles); the headline config.

## Baselines (`accel_compare`)

`accel_compare` runs all four accelerators on the same PE budget + HBM and reports
a measured per-component cycle/energy breakdown:

- **DIAMOND (diagonal, ours)** — merge-join / offset-conv on the DIA form.
- **TPU (dense MXU)** — dense n³ systolic (crosses over ~q10–12).
- **Trapezoid-MS / Trapezoid-HS** — inner-product / Gustavson sparse baselines.

**Flexagon** (SST-STONNE, bitmap O(n²), q ≤ 12) is a separate harness under
`baselines/` + `isca/flexagon/`.

## Repository layout

| path | contents |
|---|---|
| `main/` | drivers: `HBMHamiltonian` (cycle-accurate), `accel_compare` |
| `src/`, `include/` | PE mesh (`PE`, `Grid`, `Connection`, `Fifo`), reduction, Ramulator HBM |
| `verilog/` | **cycle-accurate RTL** of the datapath for energy/area (see its README) |
| `isca/` | sweep harnesses (local, not tracked) + `end_to_end_speedup.py`, provenance CSV |
| `result/` | ready result CSVs (local, not tracked) |
| `config/` | `hbm4_sota.yaml` (Ramulator HBM4) |
| `docs/` | design notes |

Analytic cost models, SLURM scripts, and third-party baselines are kept **local
and un-tracked** (`.git/info/exclude`); the repo holds the design + cycle-accurate
sources.

## Results

The three structural classes and their measured behaviour (q14, 8×8 PEs):

- **diagonal** (tfim, reg3, maxcut, tsp; D=1): levers inert except cbalance
  (perfect 64× → `cyc = mac/64`); **126×** vs Trapezoid-HS.
- **banded** (heis, qmaxcut, fermi; D~20–40): zeroskip + cbalance both pay;
  **31–37×** vs HS.
- **fill-in** (bh, chem; D grows large): DIA is a poor fit (honest scope wall);
  **~20×** vs HS at q14, un-balanced L1 OOMs.

TPU cannot reach q14–20 (OOM/timeout) — that is the crossover story.

Ready CSVs live in `result/`: `ablation_q14.csv` (L1/L2/L3 ladder),
`comparison_q14-20.csv` (DIAMOND-L3 vs TPU/HS), `end_to_end_speedup_q14.csv`.

## End-to-end speedup (QuTiP)

`isca/end_to_end_speedup.py` builds `U = exp(-iH·dt)` at 14 qubits, validates the
Taylor construction against QuTiP `expm` (agrees to 1e-12–1e-16), and compares the
accelerator (cycles / 1 GHz) to a CPU baseline and the hardware baselines:

```bash
/usr/bin/python3.11 isca/end_to_end_speedup.py        # needs qutip (or scipy fallback)
```

At q14, DIAMOND builds the propagator **370–3300× faster than CPU** and **20–126×
faster than Trapezoid-HS**.

## Energy & area (RTL)

`verilog/` is a synthesizable, faithful translation of the cycle-accurate datapath
(PE = FIFOs + merge-join comparator + MAC; NoC links; offset-space reduction +
cbalance gather tree). Each module maps to one energy component
(MAC/BUF/ROUTER/ACCUM). Run `make sim` (Icarus, dumps VCD) / `make area` (Yosys);
see `verilog/README.md`.

## Reproducibility

- Workloads are HamLib Hamiltonians in DIA form; `isca/hamlib_provenance*.csv`
  record the source HDF5 file + dataset key + fetch params for each.
- Pin: `dt=0.0012`, `num_timesteps=1000`, Ramulator HBM4 (`config/hbm4_sota.yaml`),
  gcc-12, Ramulator 2.1. Cycles are reported at 1 GHz.
