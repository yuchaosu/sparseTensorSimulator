#!/bin/bash
#SBATCH -J flex16_chain
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows16/flex_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=4
#SBATCH --exclude=c12
#SBATCH -t 23:00:00
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
source baselines/flexagon/env.sh 2>/dev/null       # module load gnu12 openmpi4 + SST paths
MAN=$REPO/isca/manifest_16x16_flex.txt             # impl|folder|file|q|K|cls
ROWS=$REPO/isca/rows16; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r impl folder file q K cls <<< "$line"
wl=${file%.txt}
dia=$HAM/$folder/$file
csr=$HAM/$folder/${wl}.csr.npz
out=$ROWS/flex_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv

csr_arg=""; [ -f "$csr" ] && csr_arg="--csr $csr"    # else run_chain derives pattern from .txt

# run_chain.py: faithful H^k@H chain, K STONNE multiplies (A densifying), summed. Emits one CSV row.
# per-step SST timeout 2h; ulimit caps host memory so densification OOM is clean (-> OOM_HOST).
( ulimit -v $((115*1024*1024)) 2>/dev/null || true
  /usr/bin/python3.11 isca/flexagon/run_chain.py --impl "$impl" \
    --dia "$dia" $csr_arg --workload "$wl" --q "$q" --K "$K" --cls "$cls" \
    --mses 256 --mem ramulator2 --repo "$REPO" --timeout-s 7200 ) > "$out" 2>$ROWS/.flog_$SLURM_ARRAY_TASK_ID
echo "done $impl $wl q$q -> $(cat $out)"
rm -f $ROWS/.flog_$SLURM_ARRAY_TASK_ID
