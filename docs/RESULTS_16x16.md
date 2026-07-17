# DIAMOND 16×16 Baseline Comparison — Results

> [!info] Scope
> All baselines on a **16×16 PE mesh (256 PEs)**, HamLib workloads built from `hamlib_provenance.csv`, Taylor order K per workload. Cycle counts are **HBM-inclusive** — `max(compute_cycles, hbm_time_ns)` at 1 GHz — with an HBM4 Ramulator-2 model (`config/hbm4_sota.yaml`). DIAMOND/TPU fold the analytic HBM number in via `-csv`; Trapezoid-HS, SegFold, and Flexagon report full-system cycles natively.

## Coverage summary (44 in-scope workloads, q ≤ 20)

| method | OK | TIMEOUT | OOM | CRASH | not run |
|---|---|---|---|---|---|
| DIAMOND-L3 | 31 | 10 | 3 | 0 | 0 |
| TPU (analytic) | 44 | 0 | 0 | 0 | 0 |
| TrapeHS/Gus | 29 | 9 | 5 | 0 | 1 |
| SegFold | 7 | 6 | 0 | 18 | 13 |
| Flex-op | 5 | 4 | 0 | 0 | 35 |
| Flex-gust | 3 | 6 | 0 | 0 | 35 |

The 6 provenance rows above q20 (`tfim_24/28`, `heis_24/28`, `bh_24`) and the dropped `O2_20` are reference/infeasible entries and are not swept.

## Per-workload cycles (HBM-inclusive)

A number is an OK cycle count; `TIMEOUT` = simulator exceeded the 22 h wall (compute-bound densification); `OOM` = exceeded the 115 GiB per-task cap; `CRASH` = tool limit hit (SegFold int32 tiling / Flexagon STONNE dim ceiling); `—` = not dispatched.

| workload | q | N | D | type | DIAMOND-L3 | TPU (analytic) | TrapeHS/Gus | SegFold | Flex-op | Flex-gust |
|---|---|---|---|---|---|---|---|---|---|---|
| Li2_14_6 | 14 | 16384 | 2683 | fill-in | 12,298,676 | 103,280,541,696 | 246,130,867 | TIMEOUT | TIMEOUT | TIMEOUT |
| O2_16_6 | 16 | 65536 | 5329 | fill-in | TIMEOUT | 6,600,290,992,128 | TIMEOUT | CRASH | — | — |
| bh_14_5 | 14 | 16384 | 143 | fill-in | 9,192,402 | 86,067,118,080 | 252,544,540 | TIMEOUT | TIMEOUT | TIMEOUT |
| bh_16_5 | 16 | 65536 | 167 | fill-in | TIMEOUT | 5,500,242,493,440 | TIMEOUT | — | — | — |
| bh_18_5 | 18 | 262144 | 192 | fill-in | TIMEOUT | 351,886,670,561,280 | TIMEOUT | CRASH | — | — |
| bh_20_5 | 20 | 1048576 | 214 | fill-in | TIMEOUT | 22,518,685,331,619,840 | TIMEOUT | TIMEOUT | — | — |
| chem_14_4 | 14 | 16384 | 1477 | fill-in | 1,788,944 | 68,853,694,464 | 62,554,658 | CRASH | TIMEOUT | TIMEOUT |
| fermi_14_4 | 14 | 16384 | 25 | banded | 116,126 | 68,853,694,464 | 9,622,569 | 3,142,480 | 36,372,084 | TIMEOUT |
| fermi_16_4 | 16 | 65536 | 29 | banded | 772,434 | 4,400,193,994,752 | 69,665,209 | TIMEOUT | — | — |
| fermi_18_5 | 18 | 262144 | 33 | banded | TIMEOUT | 351,886,670,561,280 | 935,819,419 | CRASH | — | — |
| fermi_20_5 | 20 | 1048576 | 37 | banded | TIMEOUT | 22,518,685,331,619,840 | TIMEOUT | CRASH | — | — |
| heis_14_5 | 14 | 16384 | 27 | banded | 332,942 | 86,067,118,080 | 24,312,219 | TIMEOUT | 135,079,043 | TIMEOUT |
| heis_16_5 | 16 | 65536 | 31 | banded | 2,481,113 | 5,500,242,493,440 | 184,308,715 | CRASH | — | — |
| heis_18_5 | 18 | 262144 | 35 | banded | TIMEOUT | 351,886,670,561,280 | OOM | CRASH | — | — |
| heis_20_6 | 20 | 1048576 | 39 | banded | TIMEOUT | 27,022,422,397,943,808 | TIMEOUT | CRASH | — | — |
| max3sat_14_4 | 14 | 16384 | 1 | single-diagonal | 1,664 | 68,853,694,464 | 135,204 | — | — | — |
| max3sat_16_4 | 16 | 65536 | 1 | single-diagonal | 5,265 | 4,400,193,994,752 | 428,508 | — | — | — |
| max3sat_18_4 | 18 | 262144 | 1 | single-diagonal | 26,624 | 281,509,336,449,024 | 2,162,724 | — | — | — |
| max3sat_20_4 | 20 | 1048576 | 1 | single-diagonal | 87,477 | 18,014,948,265,295,872 | 7,106,520 | — | — | — |
| maxcut_14_5 | 14 | 16384 | 1 | single-diagonal | 510 | 86,067,118,080 | 168,996 | 143,755 | 1,189,865 | 20,623,915 |
| maxcut_16_5 | 16 | 65536 | 1 | single-diagonal | 870 | 5,500,242,493,440 | 356,931 | 291,965 | — | — |
| maxcut_18_5 | 18 | 262144 | 1 | single-diagonal | 5,310 | 351,886,670,561,280 | 2,703,396 | CRASH | — | — |
| maxcut_20_5 | 20 | 1048576 | 1 | single-diagonal | 11,835 | 22,518,685,331,619,840 | 6,146,121 | CRASH | — | — |
| maxkcut_14_4 | 14 | 16384 | 1 | single-diagonal | 1,664 | 68,853,694,464 | 135,204 | — | — | — |
| maxkcut_16_4 | 16 | 65536 | 1 | single-diagonal | 4,940 | 4,400,193,994,752 | 402,108 | — | — | — |
| maxkcut_18_4 | 18 | 262144 | 1 | single-diagonal | 22,867 | 281,509,336,449,024 | 1,857,804 | — | — | — |
| maxkcut_20_4 | 20 | 1048576 | 1 | single-diagonal | 106,496 | 18,014,948,265,295,872 | — | — | — | — |
| qmaxcut_14_4 | 14 | 16384 | 27 | banded | 310,068 | 68,853,694,464 | 29,563,428 | TIMEOUT | TIMEOUT | TIMEOUT |
| qmaxcut_16_4 | 16 | 65536 | 31 | banded | 2,320,776 | 4,400,193,994,752 | 235,090,468 | CRASH | — | — |
| qmaxcut_18_4 | 18 | 262144 | 35 | banded | TIMEOUT | 281,509,336,449,024 | OOM | CRASH | — | — |
| qmaxcut_20_4 | 20 | 1048576 | 39 | banded | TIMEOUT | 18,014,948,265,295,872 | TIMEOUT | CRASH | — | — |
| reg3_14_4 | 14 | 16384 | 1 | single-diagonal | 408 | 68,853,694,464 | 135,204 | 115,004 | 951,892 | 16,499,132 |
| reg3_16_4 | 16 | 65536 | 1 | single-diagonal | 1,008 | 4,400,193,994,752 | 450,684 | 303,552 | — | — |
| reg3_18_4 | 18 | 262144 | 1 | single-diagonal | 4,248 | 281,509,336,449,024 | 2,162,724 | CRASH | — | — |
| reg3_20_4 | 20 | 1048576 | 1 | single-diagonal | 14,156 | 18,014,948,265,295,872 | TIMEOUT | CRASH | — | — |
| tfim_14_4 | 14 | 16384 | 1 | single-diagonal | 408 | 68,853,694,464 | 135,204 | 115,004 | 951,892 | 16,499,132 |
| tfim_16_4 | 16 | 65536 | 1 | single-diagonal | 1,176 | 4,400,193,994,752 | 540,708 | 493,436 | — | — |
| tfim_18_4 | 18 | 262144 | 1 | single-diagonal | 4,248 | 281,509,336,449,024 | 2,162,724 | CRASH | — | — |
| tfim_20_5 | 20 | 1048576 | 1 | single-diagonal | 20,670 | 22,518,685,331,619,840 | TIMEOUT | CRASH | — | — |
| tsp_16_4 | 16 | 65536 | 1 | single-diagonal | 1,080 | 4,400,193,994,752 | 488,304 | — | — | — |
| tsp_18_4 | 18 | 262144 | 1 | single-diagonal | 4,248 | 281,509,336,449,024 | 2,162,724 | CRASH | — | — |
| vib_14_4 | 14 | 16384 | 1805 | fill-in | OOM | 68,853,694,464 | OOM | — | — | — |
| vib_16_4 | 16 | 65536 | 2859 | fill-in | OOM | 4,400,193,994,752 | OOM | — | — | — |
| vib_18_4 | 18 | 262144 | 5367 | fill-in | OOM | 281,509,336,449,024 | OOM | — | — | — |

## Key findings

- **Only the analytic TPU "completes" everywhere** (44/44) — because it never assembles the matrix; its modeled cycle counts are astronomically large (up to ~2.7×10¹⁶), i.e. dense-GEMM cost, not a real speedup.
- **DIAMOND-L3 actually runs 31/44** and hits the wall only on dense-power blowup (large banded/fill-in) or the vib fill-in OOM — the base-dataflow densification the paper analyzes.
- **Trapezoid-HS (Gustavson)** completes fewer cells and, where it does, is 60–100× slower than DIAMOND on single-diagonal q20 and takes many real hours to simulate.
- **SegFold collapses past q14** — its dense-tile engine (`generator.cpp` densifies before simulating) overflows int32 at `⌈N/16⌉·nnz > 2³¹` (e.g. `reg3_16` OK → `reg3_18` CRASH), even on a structurally trivial single-diagonal matrix DIAMOND runs in ~4 k cycles.
- **Flexagon never leaves q14** — STONNE hard-crashes at matrix dim ≥ 2¹⁶ (q ≥ 16), an intrinsic tool ceiling; at q14 its cycle-accurate sim times out on dense powers.

## Pending: fat-node reruns (`turin128.sbatch`)

The failed **DIAMOND-L3** (13) and **Trapezoid-HS** (14) cells, plus the 10 **Flexagon** q14 TIMEOUTs, are queued for c28 ("turin128", ~1.2 TB) with the 22 h `timeout` wrapper removed (8-day wall) and the ulimit raised to 1.1 TiB — recovering the time-bound and memory-bound failures. Flexagon q ≥ 16 is **not** rerun (STONNE ceiling is unliftable). Recovered OK cells fold in automatically via `merge_16x16.py` dedup-prefer-OK.

## Reproduce

Everything derives from the one tracked input `isca/hamlib_provenance.csv`; every other artifact below is regenerated from it. On a fresh machine, start at step 0 to fetch the HamLib data from the portal.

> [!warning] Data directory
> The `diamond` binary hard-codes its data root as `/mnt/beegfs/ysu34/hamlib/` (`main/diamond.cpp`). On another computer, either build the DIA data directly under that path, or point it there with a symlink: `ln -s "$DATA" /mnt/beegfs/ysu34/hamlib`. Below, `DATA=/mnt/beegfs/ysu34/hamlib` (override to taste, then symlink).

| # | step | command | needs |
|---|---|---|---|
| 0 | download HamLib + build DIA | `python3.11 isca/fetch_from_provenance.py --download --ham "$DATA" --outdir "$DATA/dia_from_provenance" --verify` | a Python 3.11 with qiskit+scipy+h5py+numpy; net access to `portal.nersc.gov/.../hamlib`. Downloads each row's zip per its `portal_path`, reads `hdf5_key`, writes the DIA `.txt`, checks N/D/nnz vs the CSV |
| 1 | build simulators | `make` -> `outputs/{diamond,accel_compare,HBMHamiltonianAnalytic}` | `external/ramulator2/libramulator.so` (see `RAMULATOR_SETUP.md`), `config/hbm4_sota.yaml` |
| 2 | (heavy fill-in only) | `sbatch isca/build_heavy_dia.sbatch` then `python3 isca/merge_heavy_build.py` | fat node for q18-20 fill-in that OOMs a login node |
| 3 | generate manifests | `python3 isca/gen_manifest_16x16.py` | writes `manifest_16x16.txt`, `manifest_16x16_flex.txt` |
| 4a | sweep DIAMOND/TPU/TrapeHS | `sbatch --array=0-N%10 isca/sweep_16x16.sbatch` | the three C++ sims |
| 4b | sweep SegFold | `sbatch isca/sweep_16x16_segfold.sbatch` | `external/SegFold-AE` built (HBM4), `isca/segfold/run_chain_segfold.py` |
| 4c | sweep Flexagon | `sbatch isca/flexagon/sweep_16x16_flex.sbatch` | `baselines/flexagon` SST built, `isca/flexagon/run_chain.py` |
| 4d | fat-node reruns | `sbatch --array=0-36%1 isca/turin128.sbatch` | recovers failed cells on c28 (no time cap, 1.1 TiB) |
| 5 | aggregate | `python3 isca/merge_16x16.py && python3 isca/combine_16x16.py` | per-method CSVs -> `compare_16x16.csv` |
| 6 | this report | `python3 isca/gen_results_md.py` | `RESULTS_16x16.md` |

> [!note] Raw result rows (`rows16/*.csv`) and generated manifests are git-ignored (`*.csv`) / regenerable — reproduction means re-running steps 3-5, not reading committed CSVs. Only `hamlib_provenance.csv`, the scripts, `config/hbm4_sota.yaml`, and the `Makefile` are tracked.

> [!success] All families auto-download
> Every `portal_path` in the provenance was verified against the live HamLib portal (HTTP 200) and its built matrix's N/D/nnz checked against the CSV — including the chemistry-electronic molecules (`chemistry/electronic/standard/{B2,Li2,O2}.zip`) and quantum-maxcut (`binaryoptimization/qmaxcut/random/graph-star.zip`, key `graph-n-<q>`). Step 0 fetches the full q14-20 dataset with no manual sourcing.

---
*Generated 2026-07-17 from 380 result files in `rows16/` joined against `hamlib_provenance.csv`. Regenerate with `isca/gen_results_md.py` (or via `merge_16x16.py` → `combine_16x16.py`).*
