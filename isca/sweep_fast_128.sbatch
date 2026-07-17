#!/bin/bash
#SBATCH -J dia128_fast
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows128/fast_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=2
#SBATCH -t 23:00:00
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
export PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/bin:$PATH
export LD_LIBRARY_PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/lib64:$REPO/external/ramulator2:${LD_LIBRARY_PATH:-}
export RAMULATOR_HBM_CONFIG=$REPO/config/hbm4_sota.yaml
MAN=$REPO/isca/manifest_fast.txt
ROWS=$REPO/isca/rows128; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r method folder file q K cls <<< "$line"
out=$ROWS/fast_${SLURM_ARRAY_TASK_ID}.csv
wl=${file%.txt}
t0=$SECONDS

emit(){ echo "$method,$wl,$q,$K,$cls,128,$1,$2,$((SECONDS-t0))" > "$out"; }

case "$method" in
  diamond_l1|diamond_l2|diamond_l3)
    zs=1; cb=1
    [ "$method" = diamond_l1 ] && { zs=0; cb=0; }
    [ "$method" = diamond_l2 ] && { zs=1; cb=0; }
    ( ulimit -v $((115*1024*1024)) 2>/dev/null || true
      timeout 22h ./outputs/diamond -row=128 -col=128 -folder=${folder}/ -file="$file" \
        -qubit="$q" -iter="$K" -zeroskip=$zs -cbalance=$cb -verify=0 ) > $ROWS/.d_$SLURM_ARRAY_TASK_ID 2>&1
    cyc=$(grep -oE 'compute [0-9]+ cyc' $ROWS/.d_$SLURM_ARRAY_TASK_ID | grep -oE '[0-9]+' | head -1)
    if [ -n "$cyc" ]; then emit "$cyc" OK; else
      grep -qiE 'bad_alloc|terminate|Killed' $ROWS/.d_$SLURM_ARRAY_TASK_ID && emit "" OOM || emit "" TIMEOUT; fi
    rm -f $ROWS/.d_$SLURM_ARRAY_TASK_ID ;;
  traphs)
    timeout 22h ./outputs/accel_compare -folder="$HAM/$folder/" -file="$file" \
      -qubit="$q" -iter="$K" -row=128 -only=Trapezoid-HS > $ROWS/.h_$SLURM_ARRAY_TASK_ID 2>&1
    cyc=$(awk '/Trapezoid-HS \(Gustavson\)/{for(i=1;i<=NF;i++) if($i=="OK"){print $(i+1); exit}}' $ROWS/.h_$SLURM_ARRAY_TASK_ID)
    if [ -n "$cyc" ]; then emit "$cyc" OK; else
      grep -qiE 'OOM|bad_alloc' $ROWS/.h_$SLURM_ARRAY_TASK_ID && emit "" OOM || emit "" TIMEOUT; fi
    rm -f $ROWS/.h_$SLURM_ARRAY_TASK_ID ;;
  *) emit "" "BADMETHOD" ;;
esac
echo "done $method $wl q$q -> $(cat $out)"
