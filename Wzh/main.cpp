#include "./include/Grid_Update.h"
#include "./include/Connection.h"
#include "./include/TreeReducer.h"
#include "./include/Utility.h"
#include "./include/DiagonalReduction.h"

#include <iostream>
#include <vector>
#include <fstream>
#include <mutex>
#include <future>
#include <sstream>
#include <algorithm>

// Random number generator


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


void run_test_case(int i, const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
                    int& local_unsuccessful, int& local_test_cases, std::mutex& mtx, std::ofstream& out) {
    

    // Generate random matrices
    int size = i; // Fixed size for simplicity
    auto A = generate_random_matrix(size, A_offsets);
    auto B = generate_random_matrix(size, B_offsets);

    // Print generated matrices
    out << "Matrix A:\n";
    print_matrix(A, out);
    out << "Matrix B:\n";
    print_matrix(B, out);

    // Extract diagonals
    auto A_diag = extract_diagonals(A, A_offsets);
    auto B_diag = extract_diagonals(B, B_offsets);
    out << "Extracted Diagonals from A:\n";
    for (const auto& [offset, vals] : A_diag) {
        out << "Offset " << offset << ": ";
        for (const auto& [val, i, j] : vals) out << val << " ";
        out << "\n";
    }
    out << "Extracted Diagonals from B:\n";
    for (const auto& [offset, vals] : B_diag) {
        out << "Offset " << offset << ": ";
        for (const auto& [val, i, j] : vals) out << val << " ";
        out << "\n";
    }

    
    int COL = A_offsets.size();
    int ROW = B_offsets.size();

    std::vector<DiagonalReduction*> diagonalReductions;
    std::vector<std::vector<int>> reductionMap = generateReductionMap(A_offsets, B_offsets, size, diagonalReductions, out);
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

    Grid grid(ROW, COL, diagonalReductions,reductionMap, out);

    // Setup input connections
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

    // Setup TreeReducer for collecting psum + transfer
    //TreeReducer reducer(bottom_out);
    
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
    std::vector<int> A_inject_index(COL, 0);  // tracks where you are for each column
    std::vector<int> B_inject_index(ROW, 0);  // tracks where you are for each row

    bool injection_done_left = false;
    bool injection_done_top = false;
    bool injection_done = false;
    std::vector<bool> col_finish_sent(COL, false);
    std::vector<bool> row_finish_sent(ROW, false);
    while(true) {
        out << "===== Cycle " << cycle << " =====\n";
        

            // ---------- Per-column (A) Injection + Finish ----------
        for (int col = 0; col < COL; ++col) {
            if (col < A_diag_packages.size() && cycle >= col) {
                int& idx = A_inject_index[col];
                const auto& vec = A_diag_packages[col];

                if (idx < vec.size() && !top_in[col]->pendingSrc()) {
                    top_in[col]->receiveSrc(vec[idx]);
                    out << "Injecting A diagonal package at column " << col << ": " << vec[idx] << "\n";
                    ++idx;

                    if (idx == vec.size()) {
                        top_in[col]->receiveInjectionFinished(true);
                        out << "Injecting per-column finish signal at column " << col << ": true\n";
                        //col_finish_sent[col] = true;
                    }
                }
            }
        }


        // ---------- Per-row (B) Injection + Finish ----------
        for (int row = 0; row < ROW; ++row) {
            if (row < B_diag_packages.size() && cycle >= row) {
                int& idx = B_inject_index[row];
                const auto& vec = B_diag_packages[row];

                if (idx < vec.size() && !left_in[row]->pendingSrc()) {
                    left_in[row]->receiveSrc(vec[idx]);
                    out << "Injecting B diagonal package at row " << row << ": " << vec[idx] << "\n";
                    ++idx;

                    if (idx == vec.size()) {
                        left_in[row]->receiveInjectionFinished(true);
                        out << "Injecting per-row finish signal at row " << row << ": true\n";
                        //row_finish_sent[row] = true;
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
        //std::cout << "Grid state after cycle " << cycle << ":\n";
        if(injection_done && grid.isIdle()) {
            out << "All data injected and processed. Breaking out of cycle loop.\n";
            break;
        }
        ++cycle;
        out << "Left Injection Signal: " << injection_done_left << ", Top Injection Signal: " << injection_done_top << ", Grid Idle: " << grid.isIdle() << "\n";
    }

    //reducer.printResults();

    // std::cout <<"Grid Output Diagonals:\n";
    // for (const auto& [index, diag] : grid.getResults()) {
    //     std::cout << "Diagonal " << index << ": ";
    //     for (const auto& [val, i, j] : diag) {
    //         std::cout << "(" << val << ", " << i << ", " << j << ") ";
    //     }
    //     std::cout << "\n";
    // }
    std::map<int, std::vector<double>> results = addMissingZeros(grid.getResults(), size);
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
    auto C_ref = dense_matrix_multiply(A, B);
    
    std::vector<int> C_offsets = computeResultDiagonals(A_offsets, B_offsets, size);

    // Extract diagonals from reference output
    auto C_ref_diag = extract_diagonals(C_ref, C_offsets);
    std::sort(C_offsets.begin(), C_offsets.end());
    out << "Reference Output Diagonals:\n";
    for (const auto& [offset, vals] : C_ref_diag) {
        out << "Offset " << offset << ": ";
        for (const auto& [v, i, j] : vals) out << v << " ";
        out << "\n";
    }

    out << "Total Cycles: " << cycle << "\n";

    int mismatches = countMatrixMismatches(C_ref, simulated_results, out);
    //std::lock_guard<std::mutex> lock(mtx);
 
    if (mismatches > 0) {
        out << "Mismatch found! Number of mismatches: " << mismatches << "\n";
        ++local_unsuccessful;
    }
    ++local_test_cases;

    out << "========================================\n";
    std::cout << "=========================================\n";
    //delete
    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    //for (auto* conn : bottom_out) delete conn;
}

int main() {
    int total_unsuccessful = 0;
    int total_cases = 0;
    std::cout << "Starting tests for symmetric offsets...\n";
    for(int i = 100; i < 101; ++i) {
        
        std::ofstream out("outputs/output_size_" + std::to_string(i) + "_v1.txt");

        //out << "Running test iteration " << i + 1 << std::endl;

        //std::vector<std::vector<int>> offset_set = generate_symmetric_offsets(i);
        
        int local_test_cases = 0;
        int local_unsuccessful = 0;

        std::mutex mtx;
        std::vector<std::future<void>> futures;

        //for(int j = 0; j < offset_set.size(); ++j) {
        //    for(int k = 0; k < offset_set.size(); ++k) {
                //std::vector<int> A_offsets = offset_set[j];
                //std::vector<int> B_offsets = offset_set[k];
                std::vector<int> A_offsets = {-49, 0, 49};
                std::vector<int> B_offsets = {-49, 0, 49};

                out << "A offsets: ";
                for (int a : A_offsets) out << a << " ";
                out << "\nB offsets: ";
                for (int b : B_offsets) out << b << " ";
                out << "\n";

                //futures.emplace_back(std::async(std::launch::async, [&offset_set, j, k, i, &local_unsuccessful, &local_test_cases, &mtx, &out]() {
                run_test_case(i, A_offsets, B_offsets, local_unsuccessful, local_test_cases, mtx, out);
                //}));
                std::cout << "Total test cases: " << local_test_cases << "\n";
                std::cout << "Success: " << local_test_cases - local_unsuccessful << "\n";
                std::cout << "Successful Rate: " << (100.0 * (local_test_cases - local_unsuccessful) / local_test_cases) << "%" << "(" << local_test_cases - local_unsuccessful << "/" << local_test_cases << ")\n";
                if (local_unsuccessful > 0) {
                    std::cout << "Total mismatches: " << local_unsuccessful << "\n";
                } else {
                    std::cout << "All tests passed successfully!\n";
                }
        //     }
        // }

        //for (auto& f : futures) f.get();

        out << "Total test cases: " << local_test_cases << "\n";
        out << "Success: " << local_test_cases - local_unsuccessful << "\n";
        out << "Successful Rate: " << (100.0 * (local_test_cases - local_unsuccessful) / local_test_cases) << "%" << "(" << local_test_cases - local_unsuccessful << "/" << local_test_cases << ")\n";
        if (local_unsuccessful > 0) {
            out << "Total mismatches: " << local_unsuccessful << "\n";
        } else {
            out << "All tests passed successfully!\n";
        }
        total_unsuccessful += local_unsuccessful;
        total_cases += local_test_cases;

        //std::cout.rdbuf(cout_buf); // Restore original cout buffer

        // Create output directory if needed
        // fs::create_directory("outputs");

        // // Save to file
        // std::ofstream out("outputs/output_size_" + std::to_string(i) + ".txt");
        // out << oss.str();
        // out.close();
    }
    std::cout << "Total test cases: " << total_cases << "\n";
    std::cout << "Success: " << total_cases - total_unsuccessful << "\n";
    std::cout << "Successful Rate: " << (100.0 * (total_cases - total_unsuccessful) / total_cases) << "%" << "(" << total_cases - total_unsuccessful << "/" << total_cases << ")\n";

    return 0;
}
