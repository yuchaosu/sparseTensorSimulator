import numpy as np
qubit = 8  # Change this to the desired qubit number
# Load the .npz file
for index in range(1, qubit+1):  # Change this to the desired index
    data = np.load('../with_h_data/supermarq/ghz/' + str(qubit) + '/gate_'+ str(index) + '.npz')

    # List arrays (keys)
    print("Arrays in file:", data.files)

    # Pick the first array
    key = data.files[0]
    matrix = data[key]

    print("Matrix shape:", matrix.shape)

    IMAG_EPS = 1e-8
    REAL_EPS = 1e-8

    with open('./outputs/'+ str(qubit) + '/matrix_output_'+ str(index) + '.txt', 'w') as f:
        for i in range(matrix.shape[0]):
            for j in range(matrix.shape[1]):
                v = matrix[i, j]
                real_part = v.real
                imag_part = v.imag

                # If real part is effectively 0, skip this element
                if abs(real_part) < REAL_EPS:
                    continue

                # If imaginary part is effectively 0, print only real
                if abs(imag_part) < IMAG_EPS:
                    value_str = f"{real_part:.6f}"
                else:
                    value_str = f"({real_part:.6f}+{imag_part:.6f}j)"

                f.write(f"({i},{j}): {value_str}\n")

    print("Done! Non-zero entries saved to matrix_output.txt")

