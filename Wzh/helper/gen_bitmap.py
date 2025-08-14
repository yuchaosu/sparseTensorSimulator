import numpy as np
import sys
import os
import numpy as np

qubit = int(sys.argv[1])

def write_csc_files(matrix, output_dir, file_prefix=""):
    """
    Generates CSC representation of a matrix and writes index arrays to files.

    Parameters:
        matrix (np.ndarray): Matrix to convert to CSC format.
        output_dir (str): Directory to save output files.
        file_prefix (str): Optional prefix to add to filenames.
    """
    rows, cols = matrix.shape

    csc_col_ptr = [0]
    csc_row_idx = []
    nnz_csc = 0

    for j in range(cols):
        col_nnz = 0
        for i in range(rows):
            if matrix[i, j] != 0:
                csc_row_idx.append(str(i))
                col_nnz += 1
        nnz_csc += col_nnz
        csc_col_ptr.append(str(nnz_csc))

    # Ensure output directory exists
    os.makedirs(output_dir, exist_ok=True)

    csc_colptr_file = os.path.join(output_dir, f"csc_colptr{file_prefix}.in")
    csc_rowidx_file = os.path.join(output_dir, f"csc_rowidx{file_prefix}.in")

    with open(csc_colptr_file, "w") as f:
        f.write(",".join(map(str, csc_col_ptr)))
    with open(csc_rowidx_file, "w") as f:
        f.write(",".join(csc_row_idx))


def write_csr_files(matrix, output_dir, file_prefix=""):
    """
    Generates CSR representation of a matrix and writes index arrays to files.

    Parameters:
        matrix (np.ndarray): Matrix to convert to CSR format.
        output_dir (str): Directory to save output files.
        file_prefix (str): Optional prefix to add to filenames.
    """
    csr_row_ptr = [0]
    csr_col_idx = []
    nnz = 0

    for row in matrix:
        row_nnz = 0
        for j, v in enumerate(row):
            if v != 0:
                csr_col_idx.append(str(j))
                row_nnz += 1
        nnz += row_nnz
        csr_row_ptr.append(str(nnz))

    # Ensure output directory exists
    os.makedirs(output_dir, exist_ok=True)

    csr_rowptr_file = os.path.join(output_dir, f"csr_rowptr{file_prefix}.in")
    csr_colidx_file = os.path.join(output_dir, f"csr_colidx{file_prefix}.in")

    with open(csr_rowptr_file, "w") as f:
        f.write(",".join(map(str, csr_row_ptr)))
    with open(csr_colidx_file, "w") as f:
        f.write(",".join(csr_col_idx))
# Example usage:
# matrix1 = np.array(...)
# matrix2 = np.array(...)
# write_csr_csc_files(matrix1, matrix2, "./outputs/10/bitmap/", file_prefix="matrix10_")

# folder = "/mnt/beegfs/ysu34/hamlib/maxcut/"
# os.makedirs("/mnt/beegfs/ysu34/hamlib" + str(qubit) + "/bitmap/", exist_ok=True)
# filename = "H_array_complbipart-n-10_a-5_b-5_iterations_4"
# for i in range(1, qubit + 1):
#     with open(folder + filename + ".txt") as f_in, open("/mnt/beegfs/ysu34/" + str(qubit) + "/bitmap/bitmap1_" +  str(i) + ".in", "w") as f_out:
#         f_out.write(",".join("1" if float(x)!=0 else "0" for line in f_in for x in line.strip().split()))
# for i in range(1, qubit + 1):
#     with open("/mnt/beegfs/ysu34/" + str(qubit) + "/data/matrix" +  str(i) + ".txt") as f_in, open("/mnt/beegfs/ysu34/" + str(qubit) + "/bitmap/bitmap2_" +  str(i) + ".in", "w") as f_out:
#         f_out.write(",".join("1" if float(x)!=0 else "0" for line in f_in for x in line.strip().split(',')))

folder = "/mnt/beegfs/ysu34/" + str(qubit) + "/data/csr/"
os.makedirs(folder, exist_ok=True)
for i in range(1, qubit):
    # Load dense matrix from text file
    matrix1 = np.loadtxt("/mnt/beegfs/ysu34/" + str(qubit) + "/data/intermediate_ghz_result_step_" + str(i) + ".txt")
    matrix2 = np.loadtxt("/mnt/beegfs/ysu34/" + str(qubit) + "/data/matrix" +  str(i + 1) + ".txt")
    # write_csr_files(matrix1, folder, file_prefix="1"+str(i))
    # write_csr_files(matrix2, folder, file_prefix="2"+str(i))
    write_csc_files(matrix1, folder, file_prefix="1"+str(i))
    write_csc_files(matrix2, folder, file_prefix="2"+str(i))

