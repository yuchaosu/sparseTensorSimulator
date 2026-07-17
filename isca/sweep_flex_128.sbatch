#!/bin/bash
#SBATCH -J dia128_flex
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows128/flex_%A_%a.out
#SBATCH --partition=all,max
#SBATCH -N 1 -n 1 --cpus-per-task=2
#SBATCH -t 4-00:00:00
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
module load gnu12 2>/dev/null || true
export LD_LIBRARY_PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/lib64:${LD_LIBRARY_PATH:-}
MAN=$REPO/isca/manifest_flex.txt
ROWS=$REPO/isca/rows128; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line"; exit 0; }
IFS='|' read -r method folder file q K cls <<< "$line"
out=$ROWS/flex_${SLURM_ARRAY_TASK_ID}.csv
wl=${file%.txt}
impl=$( [ "$method" = flexop ] && echo op || echo gustavson )
work=/tmp/flex128_${SLURM_ARRAY_TASK_ID}_$$
t0=$SECONDS

# run_case.py emits: impl,size,step,mem,M,K,N,nnzA,nnzB,status,N_cycles,wall,mac,...
row=$(timeout 3.8d python3 isca/flexagon/run_case.py --impl "$impl" \
        --dia "$HAM/$folder/$file" --repo "$REPO" --workdir "$work" \
        --mem ramulator2 --mem-limit-gb 140 --timeout-s 324000 2>/dev/null | tail -1)
rm -rf "$work"
status=$(echo "$row" | cut -d, -f10)
cyc=$(echo "$row"    | cut -d, -f11)
[ -z "$status" ] && status=NORESULT
[ "$status" = OK ] || cyc=""
echo "$method,$wl,$q,$K,$cls,128,$cyc,$status,$((SECONDS-t0))" > "$out"
echo "done $method $wl q$q -> $(cat $out)"
