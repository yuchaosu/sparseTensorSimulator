### Standard examples ###

./outputs/matrixMultiHam -row=27 -col=606 -set=2 -way=2 -folder='' -qubit=14 -file=''-iter=5


python3.11 fetchham.py maxcut/ham-graph-complete_bipart.hdf5 complbipart-n-10_a-5_b-5 
python3.11 fetchham.py maxcut/ham-graph-regular_reg-3.hdf5 reg-3_n-10_rinst-00 

python3.11 fetchham.py maxcut/ham-graph-complete_bipart.hdf5 complbipart-n-12_a-6_b-6
python3.11 fetchham.py maxcut/ham-graph-regular_reg-3.hdf5 reg-3_n-12_rinst-00

python3.11 fetchham.py maxcut/ham-graph-complete_bipart.hdf5 complbipart-n-14_a-7_b-7
python3.11 fetchham.py maxcut/ham-graph-regular_reg-3.hdf5 reg-3_n-14_rinst-00


python3.11 fetchham.py tsp/TSP_penalties.hdf5 tsppenalty_Ncity-4_enc-stdbinary
python3.11 fetchham.py tsp/TSP_penalties.hdf5 tsppenalty_Ncity-5_enc-stdbinary

python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-0
python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-1
python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-2

python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-12_h-0
python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-12_h-0.5
python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-12_h-1

python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-14_h-0
python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-14_h-1
python3.11 fetchham.py /heis/heis.hdf5 graph-1D-grid-nonpbc-qubitnodes_Lx-14_h-2

./outputs/matrixMultiHam -set=2 -way=2 -row=1 -col=1 -iter=4 -file=''
