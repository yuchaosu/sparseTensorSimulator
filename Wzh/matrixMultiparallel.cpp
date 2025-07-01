#include "./include/Utility.h"
#include "./include/Grid.h"
#include "./include/Connection.h"
#include "./include/TreeReducer.h"
#include "./include/DiagonalReduction.h"

#include <vector>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <algorithm>

#define PARALLEL

std::vector<std::vector<int>> generateReductionMap(
    const std::vector<int>& A_offsets,
    const std::vector<int>& B_offsets,
    int size,
    std::vector<DiagonalReduction*>& diagonalReductions,
    std::ostream& out
) {
    size_t col = A_offsets.size();
    size_t row = B_offsets.size();
    std::vector<std::vector<int>> reductionMap(row, std::vector<int>(col, -1));

    std::unordered_map<int, DiagonalReduction*> reducerMap;

    std::vector<int> B_offsets_reversed = B_offsets;
    std::reverse(B_offsets_reversed.begin(), B_offsets_reversed.end()); // Reverse to match the expected order
    for (size_t i = 0; i < row; ++i) {
        for (size_t j = 0; j < col; ++j) {
            int index = A_offsets[j] + B_offsets_reversed[i];
            reductionMap[i][j] = index;

            if (reducerMap.find(index) == reducerMap.end()) {
                DiagonalReduction* reducer = new DiagonalReduction(index, out);
                reducerMap[index] = reducer;
                diagonalReductions.push_back(reducer);
                out << "Created DiagonalReduction for index: " << index << " from (A" << A_offsets[j] << ", B" << B_offsets_reversed[i] << ")" << std::endl;
                out << "DiagonalReduction " << index << " connected with PE[" << i << ", " << j << "] " << std::endl;
            }
        }
    }

    out << "Final DiagonalReduction list:\n";
    for (auto& reducer : diagonalReductions) {
        out << " - Index: " << reducer->getIndex() << "\n";
    }

    return reductionMap;
}


int main() {
    
    // Create a 5x5 test matrix
    int size = 5;

    std::ofstream out("outputs/parallel_output_size_" + std::to_string(size) + ".txt");

    std::vector<std::vector<int>> symmetric_offsets = generate_symmetric_offsets(size);
    std::vector<int> offsets_A = symmetric_offsets[1]; // Use the first set of symmetric offsets
    std::vector<int> offsets_B = symmetric_offsets[1]; // Use the second set of symmetric offsets
    std::vector<std::vector<double>> matrix_A = generate_random_matrix(size, offsets_A, 0.0, 10.0);
    std::vector<std::vector<double>> matrix_B = generate_random_matrix(size, offsets_B, 0.0, 10.0);

    // Print the generated matrices
    out << "Matrix A:\n";
    print_matrix(matrix_A, out);
    out << "Matrix B:\n";
    print_matrix(matrix_B, out);

    //Print diagonal pattern
    out << "Diagonal offsets for A: [";
    for (const auto& offset : offsets_A) {
        out << offset << " ";
    }
    out << "]\nDiagonal offsets for B: [";
    for (const auto& offset : offsets_B) {
        out << offset << " ";
    }
    out << "]\n";

    // Extract diagonals
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> diagonals_A = extract_diagonals(matrix_A, offsets_A);
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> diagonals_B = extract_diagonals(matrix_B, offsets_B);

    int num_groups = 2;
    int diag_group_A = 2;
    int diag_group_B = 2;

    std::pair<
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>,
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>
    > grouped = split_double_diagonals_by_group(diagonals_A, diagonals_B, num_groups, diag_group_A, diag_group_B);

    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>> grouped_A = grouped.first;
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>> grouped_B = grouped.second;



    // Print A
    out << "=== Grouped A (col-wise) ===\n";
    print_groups(grouped_A, out);

    // Print B
    out << "\n=== Grouped B (row-wise) ===\n";
    print_groups(grouped_B, out);

    assert(grouped_A.size() == grouped_B.size() && "Grouped A and B must have the same number of groups");

    int overall_cycles = 0;
    std::map<int, std::vector<std::tuple<double, int, int>>> overall_results;

    for (size_t i = 0; i < grouped_A.size(); i++)
    {
        for (size_t j = 0; j < grouped_A[i].size(); j++)
        {
            std::unordered_map<int, std::vector<std::tuple<double, int, int>>> A_diag = grouped_A[i][j];
            for (size_t k = 0; k < grouped_B[i].size(); k++)
            {
                std::unordered_map<int, std::vector<std::tuple<double, int, int>>> B_diag = grouped_B[i][k];

                out << "Processing Group A[" << i << "][" << j << "] and Group B[" << i << "][" << k << "]\n";

                int COL = A_diag.size();
                int ROW = B_diag.size();
                std::vector<int> A_index = rebuildOffsets(A_diag);
                std::vector<int> B_index = rebuildOffsets(B_diag);
                std::vector<DiagonalReduction*> diagonalReductions;
                std::vector<std::vector<int>> reductionMap = generateReductionMap(A_index, B_index, size, diagonalReductions, out);

                //print the reduction map
                out << "Reduction Map:\n";  
                for (const auto& row : reductionMap) {
                    for (int val : row) {
                        out << val << " ";
                    }
                    out << "\n";
                }
                out << "Diagonal Reductions Size: " << diagonalReductions.size() << "\n";
                out << "Diagonal Reductions Indices:\n";
                for (const auto& reduction : diagonalReductions) {
                    out << reduction->getIndex() << " ";
                }

                Grid grid(ROW, COL, diagonalReductions, reductionMap, out);
                std::vector<Connection*> left_in(ROW), top_in(COL);
                
                for (int i = 0; i < ROW; ++i) {
                    left_in[i] = new Connection(out);
                }
                for (int i = 0; i < COL; ++i) {
                    top_in[i] = new Connection(out);
                }
                grid.setInputConnections(top_in, left_in);

                // Setup output connections
                // std::vector<Connection*> bottom_out(COL);
                // for (int i = 0; i < COL; ++i)
                //     bottom_out[i] = new Connection(out);
                // grid.setOutputConnections(bottom_out);

                // // Setup TreeReducer for collecting psum + transfer
                // TreeReducer reducer(bottom_out);
                
                //Convert diagonals to datapackages
                std::vector<std::vector<DataPackage>> A_diag_packages = buildDatapackage(A_diag);
                std::vector<std::vector<DataPackage>> B_diag_packages = buildDatapackage(B_diag);
                std::reverse(B_diag_packages.begin(), B_diag_packages.end()); // Reverse B packages to match the expected order
                out << "A Diagonal Packages:\n";
                for (const auto& diag : A_diag_packages) {
                    for (const auto& dp : diag) {
                        out << dp << " ";
                    }
                    out << "\n";
                }
                out << "B Diagonal Packages:\n";
                for (const auto& diag : B_diag_packages) {
                    for (const auto& dp : diag) {
                        out << dp << " ";
                    }
                    out << "\n";
                }
                int cycle = 0;
                // Run simulation
                
                bool injection_done = false;
                while(true) {
                    out << "===== Cycle " << cycle << " =====\n";

                    // Inject column data (A input)
                    for (int col = 0; col < COL; ++col) {
                        if (col < A_diag_packages.size()) {
                            const auto& vec = A_diag_packages[col];
                            int index = cycle - col;
                            if (index >= 0 && index < vec.size()) {
                                if (!top_in[col]->pendingSrc()) {
                                    top_in[col]->receiveSrc(vec[index]);
                                }
                            }
                        }
                    }

                    // Inject row data (B input)
                    for (int row = 0; row < ROW; ++row) {
                        if (row < B_diag_packages.size()) {
                            const auto& vec = B_diag_packages[row];
                            int index = cycle - row;
                            if (index >= 0 && index < vec.size()) {
                                if (!left_in[row]->pendingSrc()) {
                                    left_in[row]->receiveSrc(vec[index]);
                                }
                            }
                        }
                    }
                    
                    injection_done = true;
                    for (int i = 0; i < ROW; ++i) {
                        if (left_in[i]->pendingSrc()) injection_done = false;
                    }
                    for (int i = 0; i < COL; ++i) {
                        if (top_in[i]->pendingSrc()) injection_done = false;
                    }

                    grid.cycle();
                    //reducer.cycle();

                    if(injection_done && grid.isIdle()) {
                        out << "All data injected and processed. Breaking out of cycle loop.\n";
                        break;
                    }
                    ++cycle;
                }


                std::map<int, std::vector<std::tuple<double, int, int>>> results = grid.getResults();

                //print results
                out << "Results\n";
                for (const auto& [offset, tuples] : results) {
                    out << "Offset " << offset << ": ";
                    for (const auto& [val, i, j] : tuples) {
                        out << "(" << val << ", " << i << ", " << j << ") ";
                    }
                    out << "\n";
                }

                for (const auto& [offset, tuples] : results) {
                    for (const auto& [val, i, j] : tuples) {
                        bool merged = false;
                        for (auto& existing : overall_results[offset]) {
                            if (std::get<1>(existing) == i && std::get<2>(existing) == j) {
                                std::get<0>(existing) += val;  // accumulate the value
                                merged = true;
                                break;
                            }
                        }
                        if (!merged) {
                            overall_results[offset].emplace_back(val, i, j);
                        }
                    }
                }

                //out << "Total Cycles: " << cycle << "\n";
                overall_cycles = std::max(overall_cycles, cycle);

                for (auto* conn : left_in) delete conn;
                for (auto* conn : top_in) delete conn;
                //for (auto* conn : bottom_out) delete conn;

                out << "=========================================\n";

            }
            
        }
        
    }
    std::map<int, std::vector<double>> results = addMissingZeros(overall_results, size);
    out << "Simulated Final Results:\n";
    for (const auto& [key, vec] : results) {
        out << "Diagonal " << key << ": ";
        for (double val : vec) {
            out << val << " ";
        }
        out << "\n";
    }

    std::vector<std::vector<double>> simulated_results = diagonals_to_dense(size, results);


    // Perform dense multiplication
    auto C_ref = dense_matrix_multiply(matrix_A, matrix_B);

    std::vector<int> C_offsets = computeResultDiagonals(offsets_A, offsets_B, size);

    // Extract diagonals from reference output
    auto C_ref_diag = extract_diagonals(C_ref, C_offsets);
    std::sort(C_offsets.begin(), C_offsets.end());
    out << "Reference Output Diagonals:\n";
    for (const auto& [offset, vals] : C_ref_diag) {
        out << "Offset " << offset << ": ";
        for (const auto& [v, i, j] : vals) out << v << " ";
        out << "\n";
    }



    int mismatches = countMatrixMismatches(C_ref, simulated_results, out);
    if (mismatches > 0) {
        out << "Mismatch found! Number of mismatches: " << mismatches << "\n";
    } else {
        out << "All tests passed successfully!\n";
    }

    out << "Overall Cycles: " << overall_cycles << "\n";
    

    return 0;
}

