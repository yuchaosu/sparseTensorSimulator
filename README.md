# sparseTensorSimulator
For Meshed PE
run Command
```
./stonne -DenseGEMM -M=4 -N=4 -K=16 -ms_rows=4 -ms_cols=4 -dn_bw=8 -rn_bw=16  -T_N=4 -T_M=1 -T_K=1 -accumulation_buffer=1 -rn_type="TEMPORALRN" -mn_type="OS_MESH" -mem_ctrl="TPU_OS_DENSE"
```
Meshed PE Improved Mapping

     Sparse Matrix:
     1  2  0  0  0
     3  4  5  0  0
     0  6  7  8  0
     0  0  9 10 11
     0  0  0 12 13
     diagonal:
     0  : 1 4 7 10 13
     1  : 2 5 8 11
     -1 : 3 6 9 12
     Vector:
     1 2 3 4 5
     MK:
      0   1   2
      3   4   5  
      6   7   8  
      9  10  11  
     12  13   0
     KN:
      0   1   2   3   4   
      1   2   3   4   5
      2   3   4   5   0

    None Consecutive Diagonal
     1  0  2  0  0
     0  4  0  5  0
     3  0  7  0  8
     0  6  0 10  0
     0  0  9  0 13
    diagonal:
     0  : 1 4 7 10 13
     2  : 2 5 8
     -2 : 3 6 9

     MK
      0   1   2
      0   4   5  
      3   7   8  
      6  10   0  
      9  13   0

    KN
        0   0   1   2   3   
        1   2   3   4   5
        3   4   5   0   0

    Universal Solution:
    M is the original sparse matrix size
    K is the number of the diagonals
    N is the original sparse matrix size
    The output will be a N*N matrix
    The result will lie on the main diagonal.
    Compared the calculation with the original sparse matrix, current solution only need M*K PEs.
    MK:
    Upper Diagonals, Main Diagonal, Lower Diagonals
    Each column has one diagonals
    Add X 0s in front of each upper diagonal to meet the matrix size
    Add X 0s in the end of each lower diagonal to meet the matrix size
    X is the number of the gap between current diagonal and the main diagonal
    KN:
    Upper Diagonal Corresponding Vectors（X zeros followed by the first N-X numbers of the vector）
    Original Vector
    Lower Diagonal Corresponding Vectors （The last N-X numbers of the vector followed by X zeros）
    Each row has one vector.
    X is the number of the gap between the current diagonal and the main diagonal


    Example: Above example2
    3 diagonals, original size 5

    MK:
    The first column of the MK is diagonal 2
    The second column of the MK is diagonal 0
    The third column of the MK is diagonal -2
    Since the gap between the diagonal 2 and main diagonal is 2, add 2 0 in front of the diagonal 2.(3+2=5)
    Since the gap between the diagonal -2 and main diagonal is 2, add 2 0 in the end of the diagonal -2.(3+2=5)
    
    KN:
    The first row of the KN is consists of 2 zeros and the first 3 numbers of the vector
    Original Vector
    The last row of the KN is consists of the last 3 numbers of the vector and 2 zeros
