# Ramulator 2.1 HBM setup

The HBM driver (`main/HBMHamiltonian.cpp`) uses **Ramulator 2.1** (CMU-SAFARI) as
its in-loop DRAM model, via the C++17 adapter `src/RamulatorHBM.cpp`
(`include/RamulatorHBM.h`). The Ramulator clone lives under `external/` and is
**gitignored** (large third-party tree); reproduce it as follows.

## Build Ramulator (pinned)

```bash
git clone https://github.com/CMU-SAFARI/ramulator2.git external/ramulator2
cd external/ramulator2
git checkout c5b1c3a478b68853a61a0b8f99510d0dde7e6fd0   # pinned commit used for our results
mkdir -p build && cd build
cmake ..
make -j
# produces external/ramulator2/libramulator.so
```

Requirements (present on this machine): g++ 12.4.0 (C++20), cmake 3.26.5,
python3.11 (only for regenerating configs; the checked-in config needs no python).

## HBM4 config

`config/hbm4_sota.yaml` is a pre-generated, machine-readable Ramulator config for
a **SOTA HBM4 stack**: 32 channels (full 2048-bit interface), `HBM4_32Gb_16Hi`
organization + `HBM4_8000Mbps` timing = **64 GB, 2.048 TB/s, 32 B/transaction**,
FR-FCFS + open-row + RoBaRaCoCh mapping.

To regenerate (e.g. to change capacity/rate/channels), edit and re-export:

```bash
export PYTHONPATH=external/ramulator2/python:$PYTHONPATH
python3.11 -m ramulator export <config_script.py> -o config/hbm4_sota.yaml
```

The config script builds `NUM_CHANNELS` HBM34 controllers (one per channel — that
is how Ramulator sets channel count; the DRAM spec's channel level stays 1).

## Build & run the HBM driver

```bash
make HBMHamiltonian     # compiles the adapter as C++20, links libramulator.so
./outputs/HBMHamiltonian -row=45 -col=45 -folder=heis/ -qubit=10 \
    -file=H_array_graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-0_sparse.txt -iter=2
```

The driver reports compute time, DRAM (HBM4) time, and the **memory-latency
percentage** of the whole process. Override the config path with
`RAMULATOR_HBM_CONFIG=/abs/path.yaml` if needed (default is baked in at build).
