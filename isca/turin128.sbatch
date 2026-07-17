#!/bin/bash
#SBATCH -J turin128
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/rows16/t128_%A_%a.out
#SBATCH --partition=qc,all
#SBATCH --nodelist=c28
#SBATCH -N 1 -n 1 --cpus-per-task=8
#SBATCH --exclusive
#SBATCH -t 8-00:00:00
# Fat-node (c28 / "turin128", ~1.2 TB) rerun of the 16x16 cells that DIED on the
# regular sweep -- DIAMOND-L3 (13) + Trapezoid-HS/Gustavson (14) OOM/TIMEOUT, plus the
# 10 Flexagon (STONNE) q14 TIMEOUT cells (op + gustavson). Failure modes fixed here:
#   (1) the 22h `timeout` wrapper that produced the TIMEOUTs is REMOVED -> runs complete
#       under the 8-day partition wall; Flexagon's per-power SST timeout is raised too.
#   (2) the 115 GiB ulimit that caused the OOMs is raised to ~1.1 TiB on the fat node.
# NOTE: Flexagon q>=16 is NOT here -- STONNE hard-crashes at dim>=2^16 (run_chain.py:26),
# an intrinsic tool ceiling no node can lift; only the time-bound q14 cells can recover.
# DIAMOND/Trapezoid rows -> mesh_t128_* (merge mesh_* glob + dedup-prefer-OK); Flexagon
# rows -> flex_t128_* (merge flex_* glob + flex dedup). Both fold a new OK over the old
# OOM/TIMEOUT row automatically.
# NOTE: --exclusive on the single node c28 serializes tasks (one whole-node job at a
# time); submit with a throttle (e.g. --array=0-36%1) -- the %1 is effectively forced.
set -u
REPO=/home/ysu34/DIAMOND/diamond
cd "$REPO"
MAN=${MAN:-$REPO/isca/manifest_turin128.txt}   # method|folder|file|q|K|cls
ROWS=$REPO/isca/rows16; mkdir -p "$ROWS"
HAM=/mnt/beegfs/ysu34/hamlib
MEMCAP_GB=${MEMCAP_GB:-1100}                    # fat node ~1.2 TB; leave headroom
FLEX_TIMEOUT_S=${FLEX_TIMEOUT_S:-172800}        # STONNE per-power SST timeout (2 days/step; was 7200)

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r method folder file q K cls <<< "$line"
out=$ROWS/mesh_t128_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv
wl=${file%.txt}
t0=$SECONDS

# schema: method,level,workload,q,K,class,pe,cycles,status,sec  (pe=16x16=256)
emit(){ echo "$method,$1,$wl,$q,$K,$cls,16x16,$2,$3,$((SECONDS-t0))" > "$out"; }

echo "[turin128] $(hostname) $method $wl q$q K$K cls=$cls memcap=${MEMCAP_GB}G (no time wrapper)"

case "$method" in
  diamond_l3)
    dcsv=$ROWS/.dcsv_t128_${SLURM_ARRAY_JOB_ID}_$SLURM_ARRAY_TASK_ID; rm -f "$dcsv"
    ( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
      ./outputs/diamond -row=16 -col=16 -folder=${folder}/ -file="$file" \
        -qubit="$q" -iter="$K" -zeroskip=1 -cbalance=1 -verify=0 -csv="$dcsv" ) > $ROWS/.dt128_$SLURM_ARRAY_TASK_ID 2>&1
    # HBM-inclusive runtime = max(compute_cycles, hbm_time_ns): compute from stdout,
    # hbm from the -csv row (field 13 = hbm_time_ns; @1 GHz 1 cyc == 1 ns).
    ccyc=$(grep -oE 'compute [0-9]+ cyc' $ROWS/.dt128_$SLURM_ARRAY_TASK_ID | grep -oE '[0-9]+' | head -1)
    if [ -n "$ccyc" ]; then
      hbm=$(awk -F, 'NF>=13{printf "%.0f",$13; exit}' "$dcsv" 2>/dev/null); hbm=${hbm:-0}
      cyc=$(( hbm > ccyc ? hbm : ccyc )); emit diamond_l3 "$cyc" OK
    else
      grep -qiE 'bad_alloc|terminate|Killed|std::bad' $ROWS/.dt128_$SLURM_ARRAY_TASK_ID && emit diamond_l3 "" OOM || emit diamond_l3 "" TIMEOUT; fi
    rm -f $ROWS/.dt128_$SLURM_ARRAY_TASK_ID "$dcsv" ;;
  traphs)
    ( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
      ./outputs/accel_compare -folder="$HAM/$folder/" -file="$file" \
        -qubit="$q" -iter="$K" -row=16 -only=Trapezoid-HS ) > $ROWS/.ht128_$SLURM_ARRAY_TASK_ID 2>&1
    cyc=$(awk '/Trapezoid-HS \(Gustavson\)/{for(i=1;i<=NF;i++) if($i=="OK"){print $(i+1); exit}}' $ROWS/.ht128_$SLURM_ARRAY_TASK_ID)
    if [ -n "$cyc" ]; then emit Gustavson "$cyc" OK; else
      grep -qiE 'OOM|bad_alloc|Killed' $ROWS/.ht128_$SLURM_ARRAY_TASK_ID && emit Gustavson "" OOM || emit Gustavson "" TIMEOUT; fi
    rm -f $ROWS/.ht128_$SLURM_ARRAY_TASK_ID ;;
  flex_op|flex_gust)
    # Flexagon (STONNE) q14 TIMEOUT reruns with a raised per-power SST timeout. Writes the
    # native flex-schema row to flex_t128_* (NOT the mesh schema) so merge's flex_* glob +
    # dedup folds a new OK over the old TIMEOUT. Only q14 (STONNE dim ceiling blocks q>=16).
    impl=op; [ "$method" = flex_gust ] && impl=gustavson
    source baselines/flexagon/env.sh 2>/dev/null   # module load gnu12 openmpi4 + SST paths
    fout=$ROWS/flex_t128_${SLURM_ARRAY_JOB_ID}_${SLURM_ARRAY_TASK_ID}.csv
    dia=$HAM/$folder/$file
    csr=$HAM/$folder/${wl}.csr.npz; csr_arg=""; [ -f "$csr" ] && csr_arg="--csr $csr"
    ( ulimit -v $((MEMCAP_GB*1024*1024)) 2>/dev/null || true
      /usr/bin/python3.11 isca/flexagon/run_chain.py --impl "$impl" \
        --dia "$dia" $csr_arg --workload "$wl" --q "$q" --K "$K" --cls "$cls" \
        --mses 256 --mem ramulator2 --repo "$REPO" --timeout-s "$FLEX_TIMEOUT_S" ) > "$fout" 2>$ROWS/.flt128_$SLURM_ARRAY_TASK_ID
    # run_chain emits its own row; if it produced nothing (SLURM wall / kill) synthesize a
    # TIMEOUT row in the 11-col flex schema so the cell isn't silently lost.
    [ -s "$fout" ] || echo "$impl,$wl,$q,$K,$cls,16x16,,,,TIMEOUT,$((SECONDS-t0))" > "$fout"
    rm -f $ROWS/.flt128_$SLURM_ARRAY_TASK_ID ;;
  *) emit "" "" BADMETHOD ;;
esac
echo "done $method $wl q$q -> $(cat $out)"
