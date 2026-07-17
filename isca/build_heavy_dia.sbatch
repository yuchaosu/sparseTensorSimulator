#!/bin/bash
#SBATCH -J dia_build
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/heavy_rows/build_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=4
#SBATCH --exclusive
#SBATCH --exclude=c12,c59
#SBATCH -t 12:00:00
# Build heavy (fill-in) DIA matrices from HamLib hdf5 keys on a fat compute node --
# the login node OOMs assembling vibrational/chemistry fill-in at q18-20. One task per
# manifest line: family|q|hdf5_basename|key. Emits a RESULT line into heavy_rows/;
# merge into hamlib_provenance.csv afterwards (race-safe: this never edits provenance).
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
HAM=/mnt/beegfs/ysu34/hamlib
ROWS=$REPO/isca/heavy_rows; mkdir -p "$ROWS"
MAN=${MAN:-$REPO/isca/manifest_heavy_build.txt}
MEMCAP_GB=${MEMCAP_GB:-180}

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r family q hdf5 key <<< "$line"
# resolve the hdf5 anywhere under HAM
hpath=$(find "$HAM" -maxdepth 3 -name "$hdf5" 2>/dev/null | head -1)
out=$ROWS/build_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv

( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
  timeout 11h /usr/bin/python3.11 isca/build_dia_job.py \
    --hdf5 "$hpath" --key "$key" --q "$q" --family "$family" ) 2>&1 | tee /dev/stderr | grep '^RESULT,' > "$out"
[ -s "$out" ] || echo "RESULT,$family,$q,,,,TIMEOUT/KILLED" > "$out"
echo "done $family q$q -> $(cat $out)"
