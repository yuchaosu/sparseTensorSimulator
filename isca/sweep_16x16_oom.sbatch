#!/bin/bash
#SBATCH -J dia16_oom
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows16/oom_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=2
#SBATCH --exclusive
#SBATCH --exclude=c12
#SBATCH -t 23:00:00
# Rerun of the 16x16 OOM cells (isca/manifest_16x16_oom.txt) with a HIGHER virtual-memory
# cap. The original sweep capped ulimit -v at 115 GiB; the c-series nodes are ~192 GiB
# (some far larger), so --exclusive + a 185 GiB cap gives ~70 GiB more headroom. The
# cluster has NO memory tracking (RealMemory=1), so --exclusive is the only way to make
# the ulimit meaningful; --mem/constraints cannot select nodes here.
# Unlike the original, this KEEPS the per-run capture on failure so we can tell a ulimit
# bad_alloc (needs more memory) from a real kernel SIGKILL (host OOM).
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
export PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/bin:$PATH
export LD_LIBRARY_PATH=/opt/ohpc/pub/compiler/gcc/12.4.0/lib64:$REPO/external/ramulator2:${LD_LIBRARY_PATH:-}
export RAMULATOR_HBM_CONFIG=$REPO/config/hbm4_sota.yaml
MAN=${MAN:-$REPO/isca/manifest_16x16_oom.txt}   # method|folder|file|q|K|cls (override via env)
ROWS=$REPO/isca/rows16; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib
MEMCAP_GB=${MEMCAP_GB:-185}                  # virtual-memory cap (GiB); override via env

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r method folder file q K cls <<< "$line"
out=$ROWS/mesh_oom_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv   # mesh_* -> merge globs it
wl=${file%.txt}
t0=$SECONDS

# schema: method,level,workload,q,K,class,pe,cycles,status,sec  (pe=16x16=256)
emit(){ echo "$method,$1,$wl,$q,$K,$cls,16x16,$2,$3,$((SECONDS-t0))" > "$out"; }

# Classify a failed run from its captured stdout/stderr and PRESERVE it for inspection.
# bad_alloc/terminate = hit the ulimit -v cap (needs more memory); Killed = SIGKILL
# (kernel/host OOM); else no memory marker => TIMEOUT.
classify_keep(){   # $1 = capture file
  local reason keep=$ROWS/oom_capture_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.txt
  if   grep -qiE 'bad_alloc|terminate' "$1"; then reason=OOM-ulimit
  elif grep -qi  'Killed'              "$1"; then reason=OOM-kill
  else reason=TIMEOUT; fi
  cp -f "$1" "$keep" 2>/dev/null || true
  echo "$reason"
}

case "$method" in
  diamond_l1|diamond_l2|diamond_l3)
    zs=1; cb=1; lvl=$method
    [ "$method" = diamond_l1 ] && { zs=0; cb=0; }
    [ "$method" = diamond_l2 ] && { zs=1; cb=0; }
    cap=$ROWS/.d_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID
    dcsv=$ROWS/.dcsv_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID; rm -f "$dcsv"
    ( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
      timeout 22h ./outputs/diamond -row=16 -col=16 -folder=${folder}/ -file="$file" \
        -qubit="$q" -iter="$K" -zeroskip=$zs -cbalance=$cb -verify=0 -csv="$dcsv" ) > "$cap" 2>&1
    # HBM-inclusive runtime = max(compute_cycles, hbm_time_ns[csv field 13]) @1 GHz.
    ccyc=$(grep -oE 'compute [0-9]+ cyc' "$cap" | grep -oE '[0-9]+' | head -1)
    if [ -n "$ccyc" ]; then
      hbm=$(awk -F, 'NF>=13{printf "%.0f",$13; exit}' "$dcsv" 2>/dev/null); hbm=${hbm:-0}
      cyc=$(( hbm > ccyc ? hbm : ccyc )); emit "$lvl" "$cyc" OK; rm -f "$cap" "$dcsv"; else
      emit "$lvl" "" "$(classify_keep "$cap")"; rm -f "$cap" "$dcsv"; fi ;;
  tpu)
    cap=$ROWS/.t_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID
    tcsv=$ROWS/.tcsv_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID; rm -f "$tcsv"
    timeout 22h ./outputs/HBMHamiltonianAnalytic -folder=${folder}/ -file="$file" \
      -qubit="$q" -iter="$K" -row=16 -col=16 -dataflow=tpu -csv="$tcsv" > "$cap" 2>&1
    ccyc=$(grep -oE '= [0-9]+ cyc' "$cap" | grep -oE '[0-9]+' | head -1)
    if [ -n "$ccyc" ]; then
      hbm=$(awk -F, 'NF>=13{printf "%.0f",$13; exit}' "$tcsv" 2>/dev/null); hbm=${hbm:-0}
      cyc=$(( hbm > ccyc ? hbm : ccyc )); emit tpu "$cyc" OK; rm -f "$cap" "$tcsv"; else
      emit tpu "" "$(classify_keep "$cap")"; rm -f "$cap" "$tcsv"; fi ;;
  traphs)
    cap=$ROWS/.h_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID
    ( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
      timeout 22h ./outputs/accel_compare -folder="$HAM/$folder/" -file="$file" \
        -qubit="$q" -iter="$K" -row=16 -only=Trapezoid-HS ) > "$cap" 2>&1
    cyc=$(awk '/Trapezoid-HS \(Gustavson\)/{for(i=1;i<=NF;i++) if($i=="OK"){print $(i+1); exit}}' "$cap")
    if [ -n "$cyc" ]; then emit Gustavson "$cyc" OK; rm -f "$cap"; else
      emit Gustavson "" "$(classify_keep "$cap")"; rm -f "$cap"; fi ;;
  *) emit "" "" BADMETHOD ;;
esac
echo "done $method $wl q$q cap=${MEMCAP_GB}G -> $(cat $out)"
