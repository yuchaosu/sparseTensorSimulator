#include <iostream>
#include <cmath> // For abs() function

using namespace std;

float* transposeMK(float* MK, int rows, int cols) {
    float* MK_transposed = new float[rows * cols];

    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            MK_transposed[j * rows + i] = MK[i * cols + j];
        }
    }
    return MK_transposed;
}


float* constructMK(float* diagonals, int* diagonal_index, int num_diagonals, int M) {
    unsigned int MK_size = M * num_diagonals;
    float *MK_matrix = new float[MK_size];
    int index = 0;
    int dIndex = 0;

    while (index < MK_size) {
        for (size_t j = 0; j < num_diagonals; j++) {
            int gap = abs(diagonal_index[j]);
            int num = M - gap;
            if (diagonal_index[j] < 0) {  // Upper Diagonals
                // Add zeros to the front of the diagonal
                for (int k = 0; k < gap; k++) {
                    MK_matrix[index] = 0;
                    index++;
                }

                // Add diagonal values
                for (int k = 0; k < num; k++) {
                    MK_matrix[index] = diagonals[dIndex];
                    dIndex++;
                    index++;
                }
            } else if (diagonal_index[j] == 0) {  // Main Diagonal
                for (int k = 0; k < num; k++) {
                    MK_matrix[index] = diagonals[dIndex];
                    dIndex++;
                    index++;
                }
            } else {  // Lower Diagonals
                // Add diagonal values
                for (int k = 0; k < num; k++) {
                    MK_matrix[index] = diagonals[dIndex];
                    dIndex++;
                    index++;
                }

                // Add zeros to the end of the diagonal
                for (int k = 0; k < gap; k++) {
                    MK_matrix[index] = 0;
                    index++;
                }
            }
        }
    }
    return MK_matrix;
}

float* constructKN(float* V, int* diagonal_index, int num_diagonals, int M) {
    unsigned int KN_size = M * num_diagonals;
    float* KN_matrix = new float[KN_size];
    int index = 0;

    while (index < KN_size) {
        for (size_t j = 0; j < num_diagonals; j++) {
            int gap = abs(diagonal_index[j]);
            int num = M - gap;
            if (diagonal_index[j] > 0) {  // Upper Diagonals
                // Add zeros to the front of the vector
                for (int k = 0; k < gap; k++) {
                    KN_matrix[index] = 0;
                    index++;
                }

                // Add vector values
                for (int k = 0; k < num; k++) {
                    KN_matrix[index] = V[k];
                    index++;
                }
            } else if (diagonal_index[j] == 0) {  // Main Diagonal
                for (int k = 0; k < num; k++) {
                    KN_matrix[index] = V[k];
                    index++;
                }
            } else {  // Lower Diagonals
                // Add vector values from the lower part
                for (int k = 0; k < num; k++) {
                    KN_matrix[index] = V[k + gap];
                    index++;
                }


                // Add zeros to the end of the vector
                for (int k = 0; k < gap; k++) {
                    KN_matrix[index] = 0;
                    index++;
                }
            }
        }
    }
    return KN_matrix;
}

void printMatrix(float* matrix, int rows, int cols) {
    int index = 0;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            cout << matrix[index] << " ";
            index++;
        }
        cout << endl;
    }
}


int main() {
    // Example input for the diagonals
    int M = 5;  // Size of the matrix
    int num_diagonals = 3;  // Number of diagonals
    int diagonal_index[] = {-2, 0, 2};  // Indices for diagonals (Upper, Main, Lower)
    
    // Diagonal elements
    float diagonals[] = {3.0f, 6.0f, 9.0f,
                         1.0f, 4.0f, 7.0f, 10.0f, 13.0f,
                         2.0f, 5.0f, 8.0f}; // Example diagonals (can vary)

    // Vector V for KN construction
    float V[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};  // Example vector

    // Construct MK and KN matrices
    float* MK = constructMK(diagonals, diagonal_index, num_diagonals, M);
    float* KN = constructKN(V, diagonal_index, num_diagonals, M);
    MK = transposeMK(MK, num_diagonals, M);
    cout << "MK Matrix:" << endl;
    printMatrix(MK, M, 3);

    cout << "\nKN Matrix:" << endl;
    printMatrix(KN, 3, M);

    // Clean up allocated memory
    delete[] MK;
    delete[] KN;

    return 0;
}
