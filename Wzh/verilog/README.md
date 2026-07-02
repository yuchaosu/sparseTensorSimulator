# 32×32 PE Grid (HBM-excluded)

This folder contains a synthesizable, HBM-free Verilog model of the 32×32 processing-element grid used in the sparse tensor accelerator. Every PE receives streaming `A` data from the north, streaming `B` data from the west, performs a signed MAC, forwards the operands to its south/east neighbors, and emits one product per cycle to a dedicated diagonal accumulator. Each diagonal accumulator collects the contributions that share the same `row + col` index, so a 32×32 grid produces the expected 63 diagonal reduction engines.

The RTL is intentionally free of artificial "energy" counters so that downstream flows (e.g., Synopsys power analysis) can attribute switching activity to each module directly:

- **Processing elements (`pe.v`)** implement the streaming MAC tile with full-precision (64-bit) products and simple north/west → south/east operand forwarding.
- **Diagonal accumulators (`diagonal_accumulator.v`)** accept up to 32 simultaneous PE products per cycle, sign-extend them, and maintain a running sum together with an "update" pulse.
- **Grid wrapper (`grid32x32.v`)** instantiates the 32×32 mesh (2048 PEs) plus 63 diagonal accumulators, and exposes south/east boundary streams along with the packed diagonal sums for probing.
- **System testbench (`tb_grid32x32.v`)** drives a deterministic operand ramp, waits for the pipeline to settle, and prints representative diagonal/boundary data.
- **Energy helper (`energy_breakdown.py`)** is a lightweight CLI that lets you plug in PE vs diagonal energy numbers (for example, taken from Synopsys PrimeTime PX reports) and see normalized percentages.

## Files

| File | Purpose |
| --- | --- |
| `pe.v` | Single PE tile with streaming north/west inputs, south/east forwarding, and full-precision signed MAC datapath |
| `diagonal_accumulator.v` | Parameterizable diagonal accumulator that sign-extends products, sums up to 32 lanes per cycle, and asserts an update pulse when it captures data |
| `grid32x32.v` | Structural netlist for the 32×32 grid (2048 PEs, 63 diagonal accumulators) exposing south/east boundary streams and packed diagonal sums |
| `tb_grid32x32.v` | Simple verification stimulus that generates operand ramps, waits for steady state, and prints sample diagonal/boundary data |
| `Makefile` | Convenience targets for running the simulation with Icarus Verilog |
| `energy_breakdown.py` | Post-processing script for computing PE vs diagonal percentages from user-provided energy numbers |

## Running the simulation

Requirements:

- Icarus Verilog (`iverilog`, `vvp`) for compilation/simulation
- Python 3.8+ for the energy helper

Steps:

```bash
cd verilog
make sim
```

The testbench concludes with log lines such as:

```
==== Sampled diagonal accumulations ====
Diag 0   : <value> (valid=<pulse>)
Diag 31  : <value> (valid=<pulse>)
Diag 62  : <value> (valid=<pulse>)
South lane 0 valid=<bit> data=<value>
East lane  0 valid=<bit> data=<value>
```

These messages confirm that every diagonal accumulator is instantiated (63 total) and that south/east boundary streams propagate correctly.

## Post-processing energy percentages

You can reformat numbers out of Synopsys (or any other power tool) with `energy_breakdown.py`:

```bash
cd verilog
./energy_breakdown.py --pe-energy 51.2e3 --diag-energy 14.8e3
./energy_breakdown.py --report my_power_dump.json              # expects keys `pe_energy_pj` and `diag_energy_pj`
```

The script reports each component’s absolute energy and percentage of the total, making it easy to compare PEs versus diagonal accumulators once you have tool-reported numbers.

## Customizing workloads & assumptions

- `tb_grid32x32.v` injects deterministic ramp patterns for both operand streams. You can replace the `for` loops with your own traffic generation or trace replays to mimic real workloads.
- The RTL deliberately omits any synthetic energy counters. Feed the compiled netlist and toggle activity into Synopsys PrimeTime PX (or a similar tool) to obtain power per module, then use the helper script to turn those numbers into percentages.
- The model intentionally excludes HBM behavior. Interface stubs (north/west operand buses) allow you to hook it into higher-level fabrics or trace-driven benches for more detailed studies.

## Next steps

- Map actual operand traces from the software simulator into the `north_data` / `west_data` buses to emulate true traffic.
- Calibrate the `ENERGY_PER_*` parameters using circuit-level or silicon measurements.
- Extend the diagonal accumulator outputs if you need to inspect the reduced sums for verification alongside energy estimation.
