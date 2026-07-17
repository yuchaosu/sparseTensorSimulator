#!/bin/bash
#SBATCH -J ham_convert
#SBATCH -o /home/ysu34/DIAMOND/diamond/isca/expand_logs/conv_%A_%a.out
#SBATCH --partition=turin,turin128,rome,genoa,milan,siena,cascade,skylake,normal
#SBATCH -N 1 -n 1 --cpus-per-task=4
#SBATCH -t 12:00:00
set -u
HAM=/mnt/beegfs/ysu34/hamlib
MAN=/home/ysu34/DIAMOND/diamond/isca/expand_manifest.txt
LOGS=/home/ysu34/DIAMOND/diamond/isca/expand_logs; mkdir -p "$LOGS"
mkdir -p "$HAM/dia_expand"
cd "$HAM"

line=$(sed -n "$((SLURM_ARRAY_TASK_ID+1))p" "$MAN")
[ -z "$line" ] && { echo "no line $SLURM_ARRAY_TASK_ID"; exit 0; }
IFS='|' read -r tag hdf5 key <<< "$line"

echo "task $SLURM_ARRAY_TASK_ID: tag=$tag hdf5=$hdf5 key=$key"
# fetchham_sparse_dia.py builds H via qiskit sparse path (O(#offsets*n)); names
# output <tag>_<qubits>_<K>.txt + .csr.npz + .meta. defaults 1.2/1000 match provenance dt.
HAMLIB_TAG="$tag" /usr/bin/python3.11 fetchham_sparse_dia.py "$hdf5" "$key" 1.2 1000 dia_expand
echo "exit=$? task=$SLURM_ARRAY_TASK_ID done"
