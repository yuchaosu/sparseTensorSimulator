import supermarq as sm
import qiskit
import qiskit_superstaq as qss
from qiskit import QuantumCircuit

from qiskit_aer import AerSimulator

from qiskit.converters import circuit_to_dag
# from qiskit.tools.visualization import dag_drawer
from qiskit.visualization import dag_drawer

# dag = circuit_to_dag(circ)
# dag_drawer(dag)

from pprint import pprint
import sys
import numpy
numpy.set_printoptions(threshold=sys.maxsize)
import termcolor
import os


DATA_DIR = os.getenv("QSPARSE_BENCHMARKS_WITH_H_DATA", "/mnt/beegfs/ysu34/")

os.system('color')
def matrix_print01(nparray):
    for x in nparray:
        for y in x:
            a = int(y.real != 0.0 or y.imag != 0.0)
#             print(y.real, y.imag, a)
            if a != 0:
                print(termcolor.colored("*", "red") ,end=" ", sep="")
            else:
                print(a, end=" ", sep="")
        print()
def matrix_print(nparray, size=8):
    for x in nparray:
        for y in x:
            r = '{:+02.1f}'.format(y.real)
            i = '{:+02.1f}'.format(y.imag)
            if i not in ['+0.0', '-0.0'] or r not in ['+0.0', '-0.0']:
                r = termcolor.colored(r, 'red')
                i = termcolor.colored(i, 'red')
            print("(", r, ",", i, ")",sep="", end="  ")
        print(end="\n\n")

def remove_gates(circ, to_remove=['measure', 'reset']):
    marked = []
    for ins in circ.data:
        # Access operation name and check if it's in the list of gates to remove
        if ins.operation.name in to_remove:
            marked.append(ins)
    
    # Reverse the marked list and remove from the circuit
    marked = marked[::-1]
    for each in marked:
        circ.data.remove(each)
    
    return circ


def get_circ(cir_name, n_qubits):
    if cir_name == 'ghz':
        special = sm.benchmarks.ghz.GHZ(num_qubits=n_qubits)

    elif cir_name == 'ham':
        special = sm.benchmarks.hamiltonian_simulation.HamiltonianSimulation(n_qubits, 1, 1)

    elif cir_name == 'mermin_bell':
        special = sm.benchmarks.mermin_bell.MerminBell(n_qubits)

    special_circ = sm.converters.cirq_to_qiskit(special.circuit())
    
    # removing measurements to run on unitary simulator
    special_circ.remove_final_measurements(inplace=True)
    
    ## seperate initial and final Hadamards: TODO
    special_circ = remove_gates(special_circ)
    
    return special_circ


# from qiskit.extensions.unitary import UnitaryGate
from qiskit.circuit.library import UnitaryGate
from numpy import savez
import numpy as np

def analyze_matrix_sparsity(matrix: np.ndarray):
    """
    Analyze a square matrix and compute:
    - Number of non-zero elements (NNZ)
    - Number of non-zero diagonals
    - Sparsity (% of zero entries)

    Parameters:
    - matrix: 2D numpy array (square or rectangular)

    Returns:
    - nnz: int
    - nonzero_diagonal_count: int
    - sparsity: float (0.0 to 1.0)
    """
    if not isinstance(matrix, np.ndarray):
        raise TypeError("Input must be a NumPy array")

    nnz = np.count_nonzero(matrix)
    total = matrix.size
    sparsity = 1.0 - nnz / total

    # Count non-zero diagonals
    rows, cols = matrix.shape
    min_diag = -rows + 1
    max_diag = cols - 1
    nonzero_diagonal_count = sum(
        np.any(np.diag(matrix, k=offset)) for offset in range(min_diag, max_diag + 1)
    )

    return nnz, nonzero_diagonal_count, sparsity

def write_gates_to_file(circ, n, directory):
    ins_num = 1
    for ins in circ.data:
        # print(ins)
        qc = QuantumCircuit(n)
        qc.data = [ins]
        qc.save_unitary()

        # unitary_backend = Aer.get_backend('unitary_simulator')
        unitary_backend = AerSimulator(method='unitary')

        # job = execute(qc, unitary_backend, shots=10)
        job = unitary_backend.run(qc)
        mat = job.result().get_unitary(qc, n).data

        # matrix_print01(mat)
        # print()
        savez(directory + '/gate_' + str(ins_num) + '.npz', mat)
        if ins_num == 1:
            analyze_matrix_sparsity(mat)
            print("NNZ: ", analyze_matrix_sparsity(mat)[0])
            print("Non-zero diagonals: ", analyze_matrix_sparsity(mat)[1])
            print("Sparsity: ", analyze_matrix_sparsity(mat)[2])
            break
        ins_num += 1



import os
c = sys.argv[1]
q = int(sys.argv[2])

# perform checks if we got the correct circuit and number of qubits
if c not in ['ghz', 'ham', 'mermin_bell']:
    print("Invalid circuit name")
    sys.exit(1)
if q < 1:
    print("Invalid number of qubits")
    sys.exit(1)

special_circ = get_circ(c, q)
d = DATA_DIR
if not os.path.exists(d):
	os.mkdir(d)
d = d + 'supermarq/'
if not os.path.exists(d):
	os.mkdir(d)
d = d + c + '/' 
if not os.path.exists(d):
	os.mkdir(d)
d = d + str(q)
if not os.path.exists(d):
	os.mkdir(d)
print("writing to ", d)
write_gates_to_file(special_circ, q, d)