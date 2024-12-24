#include <iostream>
#include "STONNEModel.h"
#include "types.h"
#include <chrono>
#include <assert.h>
#include "testbench.h"
#include <string>
#include <math.h>
#include <utility.h>

using namespace std;



void configDenseGEMMParameters(int argc, char *argv[], Config &stonne_cfg, std::string &layer_name, unsigned int &M, unsigned int &K, unsigned int &N, unsigned int &T_M, unsigned int &T_K, unsigned int &T_N, TileGenerator::Target &tileGeneratorTarget, TileGenerator::Generator &tileGenerator);

float* constructKN(float* V, int* diagonal_index, int num_diagonals, int M);
float* constructMK(float* diagonals, int* diagonal_index, int num_diagonals, int M);
float* transposeMK(float* MK, int rows, int cols);

bool runDenseGEMMCommand(int argc, char *argv[]);

// float* generateMatrixDense(unsigned int rows, unsigned int cols, unsigned int sparsity);

// void generateSparseDense(unsigned int rows, unsigned int cols, unsigned int sparsity);

// unsigned int* generateBitMapFromDense(float* denseMatrix, unsigned int rows, unsigned int cols, GENERATION_TYPE gen_type);

float *generateMatrixSparseFromDense(float *denseMatrix, unsigned int *bitmap, unsigned int rows, unsigned int cols, GENERATION_TYPE gen_type);

// void generateSparseDense(unsigned int rows, unsigned int cols, unsigned int sparsity);
int *generateMinorIDFromDense(float *denseMatrix, unsigned int rows, unsigned int cols, int &nnz, GENERATION_TYPE gen_type);
int *generateMajorPointerFromDense(float *denseMatrix, unsigned int rows, unsigned int cols, GENERATION_TYPE gen_type);

void printDenseMatrix(float *matrix, unsigned int rows, unsigned int cols);
void printBitMap(unsigned int *bitmap, unsigned int rows, unsigned int cols);
void printSparseMatrix(float *sparseMatrix, unsigned int *bitmap, unsigned int rows, unsigned int cols);

void printMatrix(float *matrix, int rows, int cols);

// int main(int argc, char *argv[])
// {
    // if (argc > 1)
    // { // IF there is at least one parameter, -h is checked
    //     string arg = argv[1];


    //     if ((arg == "-DenseGEMM") || (arg == "-FC"))
    //     {
    //         runDenseGEMMCommand(argc, argv);
    //     }

    //     else
    //     {
    //         std::cout << "How to use STONNE User Interface: ./" << argv[0] << " -h" << std::endl;
    //     }
    // }

    // else
    // {
    //     std::cout << "How to use STONNE User Interface: ./" << argv[0] << " -h" << std::endl;
    // }


// }



bool runDenseGEMMCommand(int argc, char *argv[])
{
    float EPSILON = 0.05;
    unsigned int MAX_RANDOM = 10; // Variable used to generate the random values
    /** Generating the inputs and outputs **/

    // Layer parameters (See MAERI paper to find out the taxonomy meaning)
    std::string layer_name = "TestLayer";
    unsigned int M = 1; // M
    unsigned int K = 3; // K
    unsigned int N = 1; // N

    // Tile parameters
    unsigned int T_M = 1; // T_M
    unsigned int T_K = 1; // T_K
    unsigned int T_N = 1; // T_N

    // TileGenerator parameters
    TileGenerator::Target tileGeneratorTarget = TileGenerator::Target::NONE;
    TileGenerator::Generator tileGenerator = TileGenerator::Generator::CHOOSE_AUTOMATICALLY;

    Config stonne_cfg; // Hardware parameters
    //    stonne_cfg.m_MSNetworkCfg.ms_size=128;
    configDenseGEMMParameters(argc, argv, stonne_cfg, layer_name, M, K, N, T_M, T_K, T_N, tileGeneratorTarget, tileGenerator); // Modify stonne_cfg and the variables according to user arguments

    // Creating arrays to store the matrices
    // unsigned int MK_size = M * K;
    // unsigned int KN_size = N * K;
    // unsigned int output_size = M * N;
    // float *MK_matrix = new float[MK_size];
    // float *KN_matrix = new float[KN_size];
    // float *output = new float[output_size];
    // float *output_cpu = new float[output_size]; // Used to store the CPU computed values to compare with the simulator version
   

    M = 5;  // Size of the matrix
    int num_diagonals = 3;  // Number of diagonals
    int diagonal_index[] = {-2, 0, 2};  // Indices for diagonals (Upper, Main, Lower)
    
    // Diagonal elements
    float diagonals[] = {3.0f, 6.0f, 9.0f,
                         1.0f, 4.0f, 7.0f, 10.0f, 13.0f,
                         2.0f, 5.0f, 8.0f}; // Example diagonals (can vary)

    // Vector V for KN construction
    float V[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};  // Example vector
    float *output = new float[M*M];
    // Construct MK and KN matrices
    float* MK = constructMK(diagonals, diagonal_index, num_diagonals, M);
    float* KN = constructKN(V, diagonal_index, num_diagonals, M);
    MK = transposeMK(MK, num_diagonals, M);

    //sequential_layer(1, K, 1, N, 1, M, 1, K, 1, MK_matrix, KN_matrix, output_cpu); // Supposes that MK=inputs (M=batch size) and KN=filters (N=number of filters)

    // Computing the CNN Layer with the simulator
    Stonne *stonne_instance = new Stonne(stonne_cfg);                                                // Creating instance of the simulator
    stonne_instance->loadDenseGEMM(layer_name, M, num_diagonals, M, MK, KN, output, CNN_DATAFLOW); // Loading the layer
    // Loads or generates a tile configuration depending on whether a TileGenerator target has been specified
    if (tileGeneratorTarget == TileGenerator::Target::NONE)
        stonne_instance->loadGEMMTile(T_N, T_K, T_M);
    else
        stonne_instance->generateTile(tileGenerator, tileGeneratorTarget);
    stonne_instance->run(); // Running the simulator
    printMatrix(output, M, M);
    /** END of configuring and running the accelerator  **/
    //
    //
    //
    /** CHECKING the results to make sure that the output is correct  **/

    // Comparing the results
    // for (int i = 0; i < output_size; i++)
    // {
    //     float difference = fabs(output[i] - output_cpu[i]);
    //     if (difference > EPSILON)
    //     {
    //         std::cout << "ERROR position " << i << ": Value ofmap simulator: " << output[i] << ". Value ofmap CPU: " << output_cpu[i] << std::endl;
    //         std::cout << "\033[1;31mT test not passed\033[0m" << std::endl;
    //         delete[] MK_matrix;
    //         delete[] KN_matrix;
    //         delete[] output;
    //         delete[] output_cpu;
    //         delete stonne_instance;
    //         assert(false); // Always false
    //     }
    // }

    // If the code does not stop then the TEST is correct
    // std::cout << "\033[1;32mTest passed correctly \033[0m" << std::endl
    //           << std::endl;

    delete[] MK;
    delete[] KN;
    delete[] output;
    //delete[] output_cpu;
    delete stonne_instance;
    return true;
}


void configDenseGEMMParameters(int argc, char *argv[], Config &stonne_cfg, std::string &layer_name, unsigned int &M, unsigned int &K, unsigned int &N, unsigned int &T_M, unsigned int &T_K, unsigned int &T_N, TileGenerator::Target &tileGeneratorTarget, TileGenerator::Generator &tileGenerator)
{
    // Parsing
    for (int i = 2; i < argc; i++)
    { // 0 is the name of the program and 1 is the execution command type
        string arg = argv[i];
        // Spliting using = character
        string::size_type pos = arg.find('=');
        if (arg.npos != pos)
        {
            string value_str = arg.substr(pos + 1);
            string name = arg.substr(0, pos);
            unsigned int value;
            if ((name != "-layer_name") && (name != "-rn_type") && (name != "-mn_type") && (name != "-mem_ctrl") && (name != "-generate_tile") && (name != "-generator"))
            { // string parameters
                value = stoi(value_str);
            }
            // Checking parameter name
            if (name == "-num_ms")
            {
                if (!ispowerof2(value))
                { // Checking that the num_ms is power of 2
                    std::cout << "Error: -num_ms must be power of 2" << std::endl;
                    exit(1);
                }
                std::cout << "Changing num_ms to " << value << std::endl; // To debug
                stonne_cfg.m_MSNetworkCfg.ms_size = value;
            }

            else if (name == "-ms_rows")
            {
                if (!ispowerof2(value))
                {
                    std::cout << "Error: -ms_rows must be power of 2" << std::endl;
                    exit(1);
                }
                std::cout << "Changing ms_rows to " << value << std::endl; // To debug
                stonne_cfg.m_MSNetworkCfg.ms_rows = value;
            }

            else if (name == "-ms_cols")
            {
                if (!ispowerof2(value))
                {
                    std::cout << "Error: -ms_cols must be power of 2" << std::endl;
                    exit(1);
                }
                std::cout << "Changing ms_cols to " << value << std::endl; // To debug
                stonne_cfg.m_MSNetworkCfg.ms_cols = value;
            }

            else if (name == "-dn_bw")
            {
                if (!ispowerof2(value))
                {
                    std::cout << "Error: -dn_bw must be power of 2" << std::endl;
                    exit(1);
                }
                std::cout << "Changing dn_bw to " << value << std::endl; // To debug
                stonne_cfg.m_SDMemoryCfg.n_read_ports = value;
            }

            else if (name == "-rn_bw")
            {
                if (!ispowerof2(value))
                {
                    std::cout << "Error: -rn_bw must be power of 2" << std::endl;
                    exit(1);
                }
                std::cout << "Changing rn_bw to " << value << std::endl;
                stonne_cfg.m_SDMemoryCfg.n_write_ports = value;
            }

            else if (name == "-accumulation_buffer")
            {
                std::cout << "Changing accumulation_buffer to " << value << std::endl;
                stonne_cfg.m_ASNetworkCfg.accumulation_buffer_enabled = value;
            }

            else if (name == "-print_stats")
            {
                if ((value != 0) && (value != 1))
                {
                    std::cout << "Error: -print_stats only supports 0 or 1" << std::endl;
                    exit(1);
                }
                std::cout << "Changing print_stats to " << value << std::endl;
                stonne_cfg.print_stats_enabled = value;
            }

            else if (name == "-rn_type")
            {
                std::cout << "Changing rn_type to " << value_str << std::endl;
                stonne_cfg.m_ASNetworkCfg.reduce_network_type = get_type_reduce_network_type(value_str);
            }

            else if (name == "-mn_type")
            {
                std::cout << "Changing mn_type to " << value_str << std::endl;
                stonne_cfg.m_MSNetworkCfg.multiplier_network_type = get_type_multiplier_network_type(value_str);
            }

            else if (name == "-mem_ctrl")
            {
                std::cout << "Changing mem_ctrl to " << value_str << std::endl;
                stonne_cfg.m_SDMemoryCfg.mem_controller_type = get_type_memory_controller_type(value_str);
            }

            // Running configuration parameters (layer)

            // Layer parameters
            else if (name == "-layer_name")
            {
                std::cout << "Changing layer_name to " << value_str << std::endl;
                layer_name = value_str;
            }

            else if (name == "-M")
            {
                std::cout << "Changing M to " << value << std::endl;
                M = value;
            }

            else if (name == "-N")
            {
                std::cout << "Changing N to " << value << std::endl;
                N = value;
            }

            else if (name == "-K")
            {
                std::cout << "Changing K to " << value << std::endl;
                K = value;
            }

            else if (name == "-T_M")
            {
                std::cout << "Changing T_M to " << value << std::endl;
                T_M = value;
            }

            else if (name == "-T_N")
            {
                std::cout << "Changing T_N to " << value << std::endl;
                T_N = value;
            }

            else if (name == "-T_K")
            {
                std::cout << "Changing T_K to " << value << std::endl;
                T_K = value;
            }

            else if (name == "-generate_tile")
            {
                std::cout << "Changing generate_tile to " << value_str << std::endl;
                tileGeneratorTarget = parseTileGeneratorTarget(value_str);
            }

            else if (name == "-generator")
            {
                std::cout << "Changing generator to " << value_str << std::endl;
                tileGenerator = parseTileGenerator(value_str);
            }

            // Parameter is not recognized
            else
            {
                std::cout << "Error: parameter " << name << " does not exist" << std::endl;
                exit(1);
            }
        }
        else
        {

            std::cout << "Error: parameter " << arg << " does not exist" << std::endl;
            exit(1);
        }
    }
}

/*
Universal Solution:
    M is the original sparse matrix size
    K is the number of the diagonals
    N is the original sparse matrix size
    The output will be a N*N matrix
    Only the first column is the result.
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
*/

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