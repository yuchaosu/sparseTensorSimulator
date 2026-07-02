#!/bin/bash
# Build the run manifest (scan beegfs) and submit the multi-node job array + a
# dependent merge job. Run this on the login node:  bash isca/submit_sweep.sh
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
HAMLIB="/mnt/beegfs/ysu34/hamlib"
MAN="$REPO/isca/manifest.txt"
GRIDS_SMALL="64 128 256"     # grids for q<=12
GRIDS_BIG="64"               # grids for q>=13 (heavy: keep it small)
ABL_GRID=128                 # grid for the tiling ablation subset

# --- build once (needs external/ramulator2/libramulator.so; see RAMULATOR_SETUP.md) ---
module load gnu12/12.4.0 2>/dev/null || true
[ -f "$REPO/external/ramulator2/libramulator.so" ] || { echo "libramulator.so missing — see RAMULATOR_SETUP.md" >&2; exit 1; }
( cd "$REPO" && make HBMHamiltonian )

infer_qubit() { awk -F'[(,)]' 'NF>=3{if($2+0>m)m=$2+0; if($3+0>m)m=$3+0} END{d=m+1; q=0; while((2^q)<d) q++; print q}' "$1"; }
iters_for()  { local q=$1; if [ "$q" -le 10 ]; then echo 4; elif [ "$q" -le 12 ]; then echo 3; elif [ "$q" -le 14 ]; then echo 2; else echo 1; fi; }
verify_for() { local q=$1; if [ "$q" -le 12 ]; then echo 1; else echo 0; fi; }

# manifest columns: folder file qubit grid iter verify reuse ctile balance
> "$MAN"
for fam in B2 BH chemistry fermi heis hnc maxcut qmaxcut tfim tsp; do
    dir="$HAMLIB/$fam"; [ -d "$dir" ] || continue
    for path in $(find "$dir" -maxdepth 1 -name '*_sparse.txt' 2>/dev/null | sort); do
        file="$(basename "$path")"; q=$(infer_qubit "$path"); [ -z "$q" ] && continue
        it=$(iters_for "$q"); vf=$(verify_for "$q")
        grids="$GRIDS_SMALL"; [ "$q" -ge 13 ] && grids="$GRIDS_BIG"
        for g in $grids; do
            echo "$fam/ $file $q $g $it $vf 1 1 0" >> "$MAN"
        done
    done
done

# tiling ablation (fast q<=10 subset, one grid, 5 configs)
ABLATION=(
  "heis/ H_array_graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-0_sparse.txt 10"
  "tfim/ H_array_graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-3_sparse.txt 10"
  "maxcut/ H_array_reg-3_n-10_rinst-00_sparse.txt 10"
  "fermi/ fh-graph-1D-grid-nonpbc-qubitnodes_Lx-5_U-2_enc-parity_sparse.txt 10"
)
for entry in "${ABLATION[@]}"; do
    set -- $entry; folder=$1 file=$2 q=$3
    [ -f "$HAMLIB/$folder$file" ] || continue
    it=$(iters_for "$q"); vf=$(verify_for "$q")
    for cfg in "0 0 0" "1 0 0" "0 1 0" "1 1 0" "1 1 1"; do
        echo "$folder $file $q $ABL_GRID $it $vf $cfg" >> "$MAN"
    done
done

N=$(wc -l < "$MAN")
echo "manifest: $N runs -> $MAN"

# submit array (spread across nodes) + dependent merge
# Pass the repo path through the environment: inside a batch job $0 points at
# SLURM's spool copy, so the scripts can't derive it themselves.
AID=$(sbatch --parsable --export=ALL,SWEEP_REPO="$REPO" --array=0-$((N-1))%64 "$REPO/isca/sweep_array.mpi")
echo "submitted array job $AID"
MID=$(sbatch --parsable --export=ALL,SWEEP_REPO="$REPO" --dependency=afterany:"$AID" "$REPO/isca/merge_sweep.mpi")
echo "submitted merge job $MID (runs after array). Final CSV: $REPO/isca/sweep_results.csv"
