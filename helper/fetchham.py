import sys
import os
import re
import h5py
import numpy as np
import scipy as sc
from qiskit.quantum_info import Statevector
from qiskit.quantum_info import SparsePauliOp
from time import perf_counter

# Check number of arguments
# if len(sys.argv) < 6:
# 	print("Usage: python app2_numpy_trotterized.py <HDF5_FILE> <HDF5_KEY> <FINAL_TIME> <NUM_TIMESTEPS> <ITRS>")
# 	print("Example: python app2_numpy_trotterized.py file.hdf5 key 1.8 200 5")
# 	sys.exit(1)

# Parse arguments

folder = "/mnt/beegfs/ysu34/"
HDF5_FILE = folder + sys.argv[1]
parent_folder = os.path.dirname(HDF5_FILE)
print("Parent folder:", parent_folder)
HDF5_KEY = sys.argv[2]
FINAL_TIME = 1.2
NUM_TIMESTEPS = 1000
ITRS = 5

# Check file existence
if not os.path.exists(HDF5_FILE):
	print("File does not exist:", HDF5_FILE)
	sys.exit(1)
if FINAL_TIME <= 0:
	print("Final time must be positive.")
	sys.exit(1)
if NUM_TIMESTEPS < 1:
	print("Number of timesteps must be at least 1.")
	sys.exit(1)
if ITRS < 1:
	print("Number of iterations must be at least 1.")
	sys.exit(1)

def read_qiskit_hdf5_new(fname_hdf5: str, key: str):
	def _generate_string(term):
		indices = [(m.group(1), int(m.group(2))) for m in re.finditer(r'([A-Z])(\d+)', term)]
		return ''.join([next((char for char, idx in indices if idx == i), 'I') for i in range(max(idx for _, idx in indices) + 1)])
	def _append_ids(pstrings):
		return [p + 'I' * (max(map(len, pstrings)) - len(p)) for p in pstrings]
	with h5py.File(fname_hdf5, 'r') as f:
		pattern = r'\(?([\d.-]+(?:[+-][\d.]+j)?)\)? \[([^\]]+)\]'
		matches = re.findall(pattern, f[key][()].decode("utf-8"))
		labels = [_generate_string(m[1]) for m in matches]
		coeffs = [complex(m[0]).real for m in matches]
		return SparsePauliOp(_append_ids(labels), coeffs)

def run_numpy_trotter(H_array, initial_state, delta_t, num_steps):
	U = sc.linalg.expm(-1j * H_array * delta_t)
	state = initial_state.data
	for _ in range(num_steps):
		state = np.dot(U, state)
	return state
def expm_negative_taylor_iterations(A_diag_real, A_diag_imag, t, tol=1e-8, max_iter=100):
    """
    Simulates Taylor expansion of e^{-i A t} for a diagonal matrix A,
    only tracks how many iterations it takes to converge.

    A_diag_real: np.array of real parts of diagonal elements
    A_diag_imag: np.array of imag parts of diagonal elements
    t: time scalar
    tol: convergence threshold
    max_iter: max number of iterations
    """

    # Construct X = -i * A * t elementwise
    diag_complex = A_diag_real + 1j * A_diag_imag
    X_diag = -1j * diag_complex * t

    # Initialize: result = I, term = I
    result_diag = np.ones_like(X_diag, dtype=np.complex128)
    term_diag = np.ones_like(X_diag, dtype=np.complex128)

    for i in range(1, max_iter + 1):
        # term = term * X / i
        term_diag = term_diag * X_diag / i
        result_diag += term_diag

        norm = np.max(np.abs(term_diag))  # max norm to check convergence
        if norm < tol:
            return i

    return max_iter



def extract_diagonal_components(H_array):
    """
    Given a dense complex matrix H_array, extract the diagonal
    and return its real and imaginary parts separately.

    Returns:
        A_diag_real: np.array of real parts of diagonal
        A_diag_imag: np.array of imaginary parts of diagonal
    """
    if not np.iscomplexobj(H_array):
        raise ValueError("Input matrix must be complex.")

    diag = np.diag(H_array)
    A_diag_real = np.real(diag)
    A_diag_imag = np.imag(diag)
    return A_diag_real, A_diag_imag

#count NNZ and non-zero diagonals

def analyze_matrix_sparsity(matrix: np.ndarray):
    """
    Analyze a square or rectangular matrix and compute:
    - Number of non-zero elements (NNZ)
    - Number of non-zero diagonals
    - List of diagonal offsets with non-zero entries
    - Sparsity (% of zero entries)
    - NNZ per non-zero diagonal
    - List of NNZ counts per non-zero diagonal (ordered by offset)

    Parameters:
    - matrix: 2D numpy array

    Returns:
    - nnz: int
    - nonzero_diagonal_count: int
    - nonzero_diagonal_offsets: List[int]
    - sparsity: float (0.0 to 1.0)
    - nnz_per_nonzero_diagonal: float
    - nnz_per_diagonal_list: List[int]
    """
    if not isinstance(matrix, np.ndarray):
        raise TypeError("Input must be a NumPy array")
    if matrix.ndim != 2:
        raise ValueError("Input must be a 2D matrix")

    rows, cols = matrix.shape
    nnz = np.count_nonzero(matrix)
    total = rows * cols
    sparsity = 1.0 - nnz / total

    nonzero_diagonal_offsets = []
    nnz_per_diagonal_list = []

    for offset in range(-rows + 1, cols):
        count = 0
        for i in range(rows):
            j = i + offset
            if 0 <= j < cols:
                if matrix[i][j] != 0:
                    count += 1
        if count > 0:
            nonzero_diagonal_offsets.append(offset)
            nnz_per_diagonal_list.append(count)

    nonzero_diagonal_count = len(nonzero_diagonal_offsets)
    nnz_per_nonzero_diagonal = nnz / nonzero_diagonal_count if nonzero_diagonal_count > 0 else 0.0

    return (
        nnz,
        nonzero_diagonal_count,
        nonzero_diagonal_offsets,
        sparsity,
        nnz_per_nonzero_diagonal,
        nnz_per_diagonal_list
    )





def write_csr_files(matrix, output_dir, file_prefix=""):
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

    with open(os.path.join(output_dir, f"csr_rowptr{file_prefix}.in"), "w") as f:
        f.write(",".join(map(str, csr_row_ptr)))
    with open(os.path.join(output_dir, f"csr_colidx{file_prefix}.in"), "w") as f:
        f.write(",".join(csr_col_idx))

def write_csc_files(matrix, output_dir, file_prefix=""):
    rows, cols = matrix.shape
    csc_col_ptr = [0]
    csc_row_idx = []
    nnz = 0

    for j in range(cols):
        col_nnz = 0
        for i in range(rows):
            if matrix[i, j] != 0:
                csc_row_idx.append(str(i))
                col_nnz += 1
        nnz += col_nnz
        csc_col_ptr.append(str(nnz))

    with open(os.path.join(output_dir, f"csc_colptr{file_prefix}.in"), "w") as f:
        f.write(",".join(map(str, csc_col_ptr)))
    with open(os.path.join(output_dir, f"csc_rowidx{file_prefix}.in"), "w") as f:
        f.write(",".join(csc_row_idx))

def write_bitmap(matrix, output_dir, file_prefix=""):
    with open(os.path.join(output_dir, f"bitmap{file_prefix}.in"), "w") as f:
        f.write(",".join("1" if val != 0 else "0" for row in matrix for val in row))

def save_formats(matrix, output_dir, step):
    file_prefix = f"_{step}"
    write_csr_files(matrix, output_dir, file_prefix)
    write_csc_files(matrix, output_dir, file_prefix)
    write_bitmap(matrix, output_dir, file_prefix)

def save_nnzd_info(matrix: np.ndarray, output_dir: str, step: int, file_handle):
    nnz, nonzero_diagonal_count, offsets, sparsity, nnzd, nnzdDiagonal = analyze_matrix_sparsity(matrix)
    file_handle.write(f"Step {step}:\n")
    file_handle.write(f"  NNZ: {nnz}\n")
    file_handle.write(f"  Non-zero diagonals: {nonzero_diagonal_count}\n")
    file_handle.write(f"  Offsets: {offsets}\n")
    file_handle.write(f"  Diagonal NNZ (NNZ per non-zero diagonal): {nnzdDiagonal}\n")
    file_handle.write(f"  Sparsity: {sparsity:.6f}\n")
    file_handle.write(f"  NNZD (NNZ per non-zero diagonal): {nnzd:.6f}\n\n")
    file_handle.flush()  # optional but useful for immediate writing to disk

def run_analysis(input_txt: str, output_dir: str, num_iters: int, label: str):
    os.makedirs(output_dir, exist_ok=True)
    log_path = os.path.join(output_dir, "nnzd_analysis.txt")
    with open(input_txt) as f:
        dense_data = [list(map(float, line.strip().split(','))) for line in f if line.strip()]
    A = np.array(dense_data)
    with open(log_path, "a") as log_file:
        log_file.write(f"Analysis for {label}:\n")
        save_nnzd_info(A, output_dir, step=0, file_handle=log_file)
        for i in range(1, num_iters):
            A = A @ A
            save_nnzd_info(A, output_dir, step=i, file_handle=log_file)
        log_file.write(f"=======================\n")

def run_power_iterations(input_txt, num_iters, output_dir):
    os.makedirs(output_dir, exist_ok=True)

    # Load matrix
    with open(input_txt) as f:
        dense_data = [list(map(float, line.strip().split(','))) for line in f if line.strip()]
    A = np.array(dense_data)
    x = np.random.randn(A.shape[1])

    # Save original matrix format (step 0)
    save_formats(A, output_dir, step=0)

    for i in range(1, num_iters):
        A = A @ A  # matrix power
        save_formats(A, output_dir, step=i)
        file = "/home/ysu34/"+ HDF5_KEY  + str(i) + ".txt"
        with open(file, "w") as f:
            #f.write("iterations: " + str(num_iterations) + "\n")
            for i in range(A.shape[0]):
                for j in range(A.shape[1]):
                    val = A[i, j]
                    if abs(val) > 1e-12:  # consider as non-zero
                        f.write(f"({i},{j}): {val:.6f}\n")


    # print(f"Saved {num_iters+1} matrix states (including original) to {output_dir}")


# Load and initialize
sp_op = read_qiskit_hdf5_new(HDF5_FILE, HDF5_KEY)
num_qubits = sp_op.num_qubits
initial_state = Statevector.from_label('10' + '0' * (num_qubits - 2))
H_array = sp_op.to_matrix()
delta_t = FINAL_TIME / NUM_TIMESTEPS

# Extract diagonal components
A_diag_real, A_diag_imag = extract_diagonal_components(H_array)
# Calculate number of iterations for convergence
num_iterations = expm_negative_taylor_iterations(A_diag_real, A_diag_imag, delta_t)

# print()
# print(f"\tnum_qubits: {num_qubits}")
# print(f"\tkey: {HDF5_KEY}")
# print(f"\tFinal time: {FINAL_TIME}")
# print(f"\tNum timesteps: {NUM_TIMESTEPS}")
# print(f"\tDelta t: {delta_t}")
# print(f"\tIterations: {ITRS}")
# print()

# Benchmark Trotterized NumPy
# for i in range(ITRS):
# 	t_start = perf_counter()
# 	final_state = run_numpy_trotter(H_array, initial_state, delta_t, NUM_TIMESTEPS)
# 	t_elapsed = perf_counter() - t_start
# 	print(f"numpy-trotter,{num_qubits},{HDF5_KEY},{t_elapsed}", flush=True)

# Extract only the real part of the Hamiltonian
H_real = H_array.real
H_imag = H_array.imag

# Save dense real matrix as text
np.savetxt("/mnt/beegfs/ysu34/nouse/"+ HDF5_KEY +"_iterations_" + str(num_iterations) + ".txt", H_real, fmt="%.6f", delimiter=",")

# # Save sparse format (non-zero real entries only)
file = "/mnt/beegfs/ysu34/nouse/"+ HDF5_KEY  + "_sparse.txt"
count_imag = 0
with open(file, "w") as f:
    #f.write("iterations: " + str(num_iterations) + "\n")
    for i in range(H_real.shape[0]):
        for j in range(H_real.shape[1]):
            val = H_real[i, j]
            if H_imag[i, j] == 0:
                count_imag += 1
            if abs(val) > 1e-12:  # consider as non-zero
                f.write(f"({i},{j}): {val:.6f}\n")
print(f"Saved sparse real matrix to: {file} (ignored {count_imag} non-zero imaginary entries)")
# nnz, nonzero_diagonal_count, offsets, sparsity = analyze_matrix_sparsity(H_array)
# print(f"Matrix sparsity analysis for {HDF5_KEY}:")
# print(f"\tNumber of non-zero elements (NNZ): {nnz}")
# print(f"\tNumber of non-zero diagonals: {nonzero_diagonal_count}")
# print(f"\tDiagonal Sparsity: {(1 - nonzero_diagonal_count / (H_array.shape[0] * 2 - 1)):.4f} (1.0 means all diagonals are non-zero)")
# print(f"\tSparsity: {sparsity:.6f} (0.0 means dense, 1.0 means empty)")
# print("\tNon-zero diagonal offsets:", offsets)
# print("\tNum iterations:", num_iterations)
# print("Save sparse matrix to:", file)

# === CONFIGURATION ===
input_matrix_file = "/mnt/beegfs/ysu34/nouse/"+ HDF5_KEY +"_iterations_" + str(num_iterations) + ".txt"
output_dir = "/mnt/beegfs/ysu34/nouse/"
iterations = num_iterations

run_power_iterations(input_matrix_file, iterations, output_dir)
# run_analysis(input_matrix_file, "./", iterations, HDF5_FILE+HDF5_KEY)