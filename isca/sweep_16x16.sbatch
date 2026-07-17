#!/bin/bash
#SBATCH -J dia16_mesh
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows16/mesh_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=2
#SBATCH --exclusive
#SBATCH --exclude=c12
#SBATCH -t 23:00:00
# --exclusive: this cluster reports RealMemory=1MB (memory untracked), so without a whole-node
# reservation SLURM packs many tasks per node and the DIAMOND-l1 memory hogs OOM-kill their
# neighbors (spurious OOM/sec=0 TIMEOUT). One task per node keeps the 115 GiB ulimit meaningful.
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
export PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/bin:$PATH
export LD_LIBRARY_PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/lib64:$REPO/external/ramulator2:${LD_LIBRARY_PATH:-}
export RAMULATOR_HBM_CONFIG=$REPO/config/hbm4_sota.yaml
MAN=${MAN:-$REPO/isca/manifest_16x16.txt}  # method|folder|file|q|K|cls (override via env; diamond_l1/l2/l3|tpu|traphs)
ROWS=$REPO/isca/rows16; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r method folder file q K cls <<< "$line"
out=$ROWS/mesh_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv
wl=${file%.txt}
t0=$SECONDS

# schema: method,level,workload,q,K,class,pe,cycles,status,sec  (pe=16x16=256)
emit(){ echo "$method,$1,$wl,$q,$K,$cls,16x16,$2,$3,$((SECONDS-t0))" > "$out"; }

case "$method" in
  diamond_l1|diamond_l2|diamond_l3)
    zs=1; cb=1; lvl=$method
    [ "$method" = diamond_l1 ] && { zs=0; cb=0; }
    [ "$method" = diamond_l2 ] && { zs=1; cb=0; }
    dcsv=$ROWS/.dcsv_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID; rm -f "$dcsv"
    ( ulimit -v $((115*1024*1024)) 2>/dev/null || true
      timeout 22h ./outputs/diamond -row=16 -col=16 -folder=${folder}/ -file="$file" \
        -qubit="$q" -iter="$K" -zeroskip=$zs -cbalance=$cb -verify=0 -csv="$dcsv" ) > $ROWS/.d_$SLURM_ARRAY_TASK_ID 2>&1
    # HBM-inclusive runtime = max(compute_cycles, hbm_time_ns): compute from stdout (success
    # signal), hbm from the -csv row (field 13 = hbm_time_ns; @1 GHz 1 cyc == 1 ns).
    ccyc=$(grep -oE 'compute [0-9]+ cyc' $ROWS/.d_$SLURM_ARRAY_TASK_ID | grep -oE '[0-9]+' | head -1)
    if [ -n "$ccyc" ]; then
      hbm=$(awk -F, 'NF>=13{printf "%.0f",$13; exit}' "$dcsv" 2>/dev/null); hbm=${hbm:-0}
      cyc=$(( hbm > ccyc ? hbm : ccyc )); emit "$lvl" "$cyc" OK
    else
      grep -qiE 'bad_alloc|terminate|Killed' $ROWS/.d_$SLURM_ARRAY_TASK_ID && emit "$lvl" "" OOM || emit "$lvl" "" TIMEOUT; fi
    rm -f $ROWS/.d_$SLURM_ARRAY_TASK_ID "$dcsv" ;;
  tpu)
    tcsv=$ROWS/.tcsv_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID; rm -f "$tcsv"
    timeout 22h ./outputs/HBMHamiltonianAnalytic -folder=${folder}/ -file="$file" \
      -qubit="$q" -iter="$K" -row=16 -col=16 -dataflow=tpu -csv="$tcsv" > $ROWS/.t_$SLURM_ARRAY_TASK_ID 2>&1
    # HBM-inclusive runtime = max(compute, hbm_time_ns): fold the analytic HBM number in.
    ccyc=$(grep -oE '= [0-9]+ cyc' $ROWS/.t_$SLURM_ARRAY_TASK_ID | grep -oE '[0-9]+' | head -1)
    if [ -n "$ccyc" ]; then
      hbm=$(awk -F, 'NF>=13{printf "%.0f",$13; exit}' "$tcsv" 2>/dev/null); hbm=${hbm:-0}
      cyc=$(( hbm > ccyc ? hbm : ccyc )); emit tpu "$cyc" OK
    else
      grep -qiE 'bad_alloc|terminate|Killed|OOM' $ROWS/.t_$SLURM_ARRAY_TASK_ID && emit tpu "" OOM || emit tpu "" TIMEOUT; fi
    rm -f $ROWS/.t_$SLURM_ARRAY_TASK_ID "$tcsv" ;;
  traphs)
    ( ulimit -v $((115*1024*1024)) 2>/dev/null || true
      timeout 22h ./outputs/accel_compare -folder="$HAM/$folder/" -file="$file" \
        -qubit="$q" -iter="$K" -row=16 -only=Trapezoid-HS ) > $ROWS/.h_$SLURM_ARRAY_TASK_ID 2>&1
    cyc=$(awk '/Trapezoid-HS \(Gustavson\)/{for(i=1;i<=NF;i++) if($i=="OK"){print $(i+1); exit}}' $ROWS/.h_$SLURM_ARRAY_TASK_ID)
    if [ -n "$cyc" ]; then emit Gustavson "$cyc" OK; else
      grep -qiE 'OOM|bad_alloc|Killed' $ROWS/.h_$SLURM_ARRAY_TASK_ID && emit Gustavson "" OOM || emit Gustavson "" TIMEOUT; fi
    rm -f $ROWS/.h_$SLURM_ARRAY_TASK_ID ;;
  *) emit "" "" BADMETHOD ;;
esac
echo "done $method $wl q$q -> $(cat $out)"
