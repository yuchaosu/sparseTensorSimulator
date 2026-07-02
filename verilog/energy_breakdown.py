#!/usr/bin/env python3
"""Energy breakdown helper for the 32x32 systolic grid.

Point this script at a JSON file with fields such as ``pe_energy_pj`` and
``diag_energy_pj`` (for example, exported from Synopsys PrimeTime PX) or pass
the numbers directly on the command line. It then reports absolute energies and
percentages for PEs and diagonal accumulators individually.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, Optional


def load_report(path: Path) -> Optional[Dict[str, float]]:
    if not path.exists():
        return None
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Summarize PE vs diagonal accumulator energy usage.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=Path("energy_report.json"),
        help="Path to the JSON report emitted by tb_grid32x32.v",
    )
    parser.add_argument("--pe-energy", type=float, help="Override PE energy in picojoules")
    parser.add_argument("--diag-energy", type=float, help="Override diagonal accumulator energy in picojoules")
    parser.add_argument("--pe-energy-per-mac", type=float, default=10.0, help="Energy per PE MAC (pJ) for estimating from counts")
    parser.add_argument("--diag-energy-per-reduction", type=float, default=4.0, help="Energy per diagonal reduction (pJ)")
    parser.add_argument("--mac-count", type=float, help="Override total PE MAC count")
    parser.add_argument("--reduce-count", type=float, help="Override total diagonal reduction count")

    args = parser.parse_args()

    report = load_report(args.report)

    pe_energy = args.pe_energy
    diag_energy = args.diag_energy
    mac_count = args.mac_count
    reduce_count = args.reduce_count

    if report:
        pe_energy = pe_energy if pe_energy is not None else report.get("pe_energy_pj")
        diag_energy = diag_energy if diag_energy is not None else report.get("diag_energy_pj")
        mac_count = mac_count if mac_count is not None else report.get("pe_mac_count")
        reduce_count = reduce_count if reduce_count is not None else report.get("diag_reduce_count")

    if pe_energy is None and mac_count is not None:
        pe_energy = mac_count * args.pe_energy_per_mac
    if diag_energy is None and reduce_count is not None:
        diag_energy = reduce_count * args.diag_energy_per_reduction

    if pe_energy is None or diag_energy is None:
        raise SystemExit("Provide either --report or explicit energy values/counts")

    total_energy = pe_energy + diag_energy
    if total_energy <= 0:
        raise SystemExit("Total energy must be positive")

    pe_pct = (pe_energy / total_energy) * 100.0
    diag_pct = (diag_energy / total_energy) * 100.0

    header = f"{'Component':<20}{'Energy (pJ)':>15}{'Percent':>12}"
    print(header)
    print("-" * len(header))
    print(f"{'Processing Elements':<20}{pe_energy:>15.2f}{pe_pct:>11.2f}%")
    print(f"{'Diagonal Accumulators':<20}{diag_energy:>15.2f}{diag_pct:>11.2f}%")
    print("-" * len(header))
    print(f"{'Total':<20}{total_energy:>15.2f}{100.00:>11.2f}%")


if __name__ == "__main__":
    main()
