#!/bin/bash
#SBATCH -J segfold16_chain
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows16/segfold_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=4
#SBATCH --exclusive
#SBATCH --exclude=c12,c59
#SBATCH -t 23:00:00
# SegFold (csegfold SpMSpM) baseline, faithful H^k@H chain (isca/segfold/run_chain_segfold.py),
# 16x16 array + HBM4 DRAM (isca/segfold/segfold_16x16_hbm4.yaml -> config/hbm4_sota.yaml, the
# same Ramulator2 HBM4 DIAMOND uses). --exclusive + ulimit so densification host-OOM is clean.
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
export PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/bin:$PATH
export LD_LIBRARY_PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/lib64:${LD_LIBRARY_PATH:-}
MAN=$REPO/isca/manifest_16x16_segfold.txt          # impl|folder|file|q|K|cls
ROWS=$REPO/isca/rows16; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib
MEMCAP_GB=${MEMCAP_GB:-115}

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r impl folder file q K cls <<< "$line"
wl=${file%.txt}
dia=$HAM/$folder/$file
csr=$HAM/$folder/${wl}.csr.npz
out=$ROWS/segfold_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv

csr_arg=""; [ -f "$csr" ] && csr_arg="--csr $csr"    # else derive pattern from the .txt

# run_chain_segfold.py: K csegfold SpMSpM multiplies (A=H^k densifying, B=H), summed cycles.
# Emits one flex-schema CSV row (impl=segfold). Per-step csegfold timeout 2h; ulimit caps host
# memory so scipy densification OOM is clean (-> OOM_HOST), matching the DIAMOND/Flexagon sweeps.
( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
  /usr/bin/python3.11 isca/segfold/run_chain_segfold.py --impl "$impl" \
    --dia "$dia" $csr_arg --workload "$wl" --q "$q" --K "$K" --cls "$cls" \
    --timeout-s 7200 ) > "$out" 2>$ROWS/.slog_$SLURM_ARRAY_TASK_ID
echo "done $impl $wl q$q -> $(cat $out)"
rm -f $ROWS/.slog_$SLURM_ARRAY_TASK_ID
