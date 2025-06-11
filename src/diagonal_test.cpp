#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include "STONNEModel.h"
#include "types.h"
#include "Config.h"

// Function to generate a random matrix with non-zero values only on specified diagonals
float* generateDiagonalMatrix(unsigned int rows, unsigned int cols, const std::vector<int>& offsets, unsigned int min_val = 1, unsigned int max_val = 10) {
    float* matrix = new float[rows * cols]();  // Initialize with zeros

    for (int offset : offsets) {
        int start_i, end_i;

        if (offset >= 0) {
            // Diagonal is offset columns to the right of main
            start_i = 0;
            end_i = std::min(rows - 1, cols - 1 - offset);
        } else {
            // Diagonal is -offset rows below main
            start_i = -offset;
            end_i = std::min(rows - 1, cols - 1 + start_i);
        }

        for (int i = start_i; i <= end_i; i++) {
            int j = i + offset;
            if (j >= 0 && j < cols) {
                matrix[i * cols + j] = rand() % (max_val - min_val + 1) + min_val;
            }
        }
    }

    return matrix;
}

// Function to print a matrix
void printMatrix(float* matrix, unsigned int rows, unsigned int cols) {
    for (unsigned int i = 0; i < rows; i++) {
        for (unsigned int j = 0; j < cols; j++) {
            std::cout << matrix[i * cols + j] << " ";
        }
        std::cout << std::endl;
    }
}

// Function to compute matrix multiplication for verification
float* computeMatrixMultiplication(float* A, float* B, unsigned int M, unsigned int K, unsigned int N) {
    float* C = new float[M * N]();  // Initialize with zeros

    for (unsigned int i = 0; i < M; i++) {
        for (unsigned int j = 0; j < N; j++) {
            for (unsigned int k = 0; k < K; k++) {
                C[i * N + j] += A[i * K + k] * B[k * N + j];
            }
        }
    }

    return C;
}

// Function to verify the results
bool verifyResults(float* expected, float* actual, unsigned int size, float tolerance = 1e-5) {
    for (unsigned int i = 0; i < size; i++) {
        if (std::abs(expected[i] - actual[i]) > tolerance) {
            std::cout << "Mismatch at index " << i << ": Expected " << expected[i] << ", Got " << actual[i] << std::endl;
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    // Seed random number generator
    srand(time(NULL));

    // Parse command line arguments
    std::string config_file = "stonne.cfg";
    unsigned int M = 5;  // Rows of A
    unsigned int K = 5;  // Columns of A, Rows of B
    unsigned int N = 5;  // Columns of B

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-config" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "-M" && i + 1 < argc) {
            M = std::stoi(argv[++i]);
        } else if (arg == "-K" && i + 1 < argc) {
            K = std::stoi(argv[++i]);
        } else if (arg == "-N" && i + 1 < argc) {
            N = std::stoi(argv[++i]);
        }
    }

    std::cout << "Running diagonal dataflow test with matrix dimensions:" << std::endl;
    std::cout << "A: " << M << "x" << K << std::endl;
    std::cout << "B: " << K << "x" << N << std::endl;

    // Define diagonal offsets for matrices A and B
    std::vector<int> A_offsets = {-1, 0, 1};  // Main diagonal and adjacent diagonals
    std::vector<int> B_offsets = {-1, 0, 1};  // Main diagonal and adjacent diagonals

    // Create fixed matrices for testing
    float* A = new float[M * K]();
    float* B = new float[K * N]();

    // Set values on the main diagonal (offset 0)
    for (unsigned int i = 0; i < std::min(M, K); i++) {
        A[i * K + i] = i + 1;  // 1, 2, 3, 4, 5 on the diagonal
    }

    // Set values on the diagonal above main (offset 1)
    for (unsigned int i = 0; i < std::min(M, K-1); i++) {
        A[i * K + (i+1)] = 1;  // 1's on the diagonal above main
    }

    // Set values on the diagonal below main (offset -1)
    for (unsigned int i = 1; i < std::min(M, K); i++) {
        A[i * K + (i-1)] = 2;  // 2's on the diagonal below main
    }

    // Set values on the main diagonal (offset 0)
    for (unsigned int i = 0; i < std::min(K, N); i++) {
        B[i * N + i] = i + 1;  // 1, 2, 3, 4, 5 on the diagonal
    }

    // Set values on the diagonal above main (offset 1)
    for (unsigned int i = 0; i < std::min(K, N-1); i++) {
        B[i * N + (i+1)] = 1;  // 1's on the diagonal above main
    }

    // Set values on the diagonal below main (offset -1)
    for (unsigned int i = 1; i < std::min(K, N); i++) {
        B[i * N + (i-1)] = 2;  // 2's on the diagonal below main
    }

    // Print input matrices
    std::cout << "Matrix A:" << std::endl;
    printMatrix(A, M, K);
    std::cout << std::endl;

    std::cout << "Matrix B:" << std::endl;
    printMatrix(B, K, N);
    std::cout << std::endl;

    // Compute expected result using standard matrix multiplication
    float* expected_C = computeMatrixMultiplication(A, B, M, K, N);

    // Print expected output
    std::cout << "Expected Output Matrix C:" << std::endl;
    printMatrix(expected_C, M, N);
    std::cout << std::endl;

    // Allocate memory for the output matrix
    float* C = new float[M * N]();

    // Create a default STONNE configuration
    Config stonne_cfg;

    // Configure for diagonal dataflow
    stonne_cfg.m_MSNetworkCfg.ms_size = 16;
    stonne_cfg.m_MSNetworkCfg.ms_rows = 4;
    stonne_cfg.m_MSNetworkCfg.ms_cols = 4;
    stonne_cfg.m_MSNetworkCfg.multiplier_network_type = OS_MESH;

    stonne_cfg.m_ASNetworkCfg.reduce_network_type = ASNETWORK;
    stonne_cfg.m_ASNetworkCfg.accumulation_buffer_enabled = 1;

    stonne_cfg.m_SDMemoryCfg.n_read_ports = 4;
    stonne_cfg.m_SDMemoryCfg.n_write_ports = 4;
    stonne_cfg.m_SDMemoryCfg.write_buffer_capacity = 64;
    stonne_cfg.m_SDMemoryCfg.port_width = 8;

    stonne_cfg.m_LookUpTableCfg.port_width = 8;

    stonne_cfg.m_MSwitchCfg.forwarding_ports = 1;
    stonne_cfg.m_MSwitchCfg.buffers_capacity = 2;
    stonne_cfg.m_MSwitchCfg.port_width = 8;

    stonne_cfg.m_ASwitchCfg.forwarding_ports = 1;
    stonne_cfg.m_ASwitchCfg.buffers_capacity = 2;
    stonne_cfg.m_ASwitchCfg.port_width = 8;

    stonne_cfg.m_DSwitchCfg.forwarding_ports = 1;
    stonne_cfg.m_DSwitchCfg.buffers_capacity = 2;
    stonne_cfg.m_DSwitchCfg.port_width = 8;

    // Create STONNE instance
    Stonne* stonne_instance = new Stonne(stonne_cfg);

    // Load the diagonal GEMM
    stonne_instance->loadDiagonalGEMM("diagonal_test", N, K, M, A, B, C, A_offsets, B_offsets);

    // Run the simulation
    stonne_instance->run();

    // Print actual output
    std::cout << "Actual Output Matrix C:" << std::endl;
    printMatrix(C, M, N);
    std::cout << std::endl;

    // The DiagonalSDMemory controller now computes the result directly

    // Verify results
    bool success = verifyResults(expected_C, C, M * N);
    if (success) {
        std::cout << "Test PASSED: Results match expected output." << std::endl;
    } else {
        std::cout << "Test FAILED: Results do not match expected output." << std::endl;
    }

    // Clean up
    delete stonne_instance;
    delete[] A;
    delete[] B;
    delete[] C;
    delete[] expected_C;

    return success ? 0 : 1;
}
