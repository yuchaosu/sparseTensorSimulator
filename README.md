# sparseTensorSimulator
For Meshed PE
run Command
```
./stonne -DenseGEMM -M=4 -N=4 -K=16 -ms_rows=4 -ms_cols=4 -dn_bw=8 -rn_bw=16  -T_N=4 -T_M=1 -T_K=1 -accumulation_buffer=1 -rn_type="TEMPORALRN" -mn_type="OS_MESH" -mem_ctrl="TPU_OS_DENSE"
```
Meshed PE Improved Mapping
