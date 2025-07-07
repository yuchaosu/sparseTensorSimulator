#include "./include/Grid.h"
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

std::map<int, std::vector<std::tuple<double, int, int>>> combineMaps(const std::map<int, std::vector<std::tuple<double, int, int>>>& mapA, const std::map<int, std::vector<std::tuple<double, int, int>>>& mapB) {
    std::map<int, std::vector<std::tuple<double, int, int>>> result = mapA;  // Start with all entries from mapA

    for (const auto& [key, vecB] : mapB) {
        auto& vecRes = result[key];  // Creates empty vector if key does not exist

        // For faster lookup, build an index of (i,j) -> position in vecRes
        std::map<std::pair<int,int>, size_t> indexMap;
        for (size_t i = 0; i < vecRes.size(); ++i) {
            int row = std::get<1>(vecRes[i]);
            int col = std::get<2>(vecRes[i]);
            indexMap[{row, col}] = i;
        }

        // Process each tuple in vecB
        for (const auto& tupB : vecB) {
            int rowB = std::get<1>(tupB);
            int colB = std::get<2>(tupB);
            auto it = indexMap.find({rowB, colB});
            if (it != indexMap.end()) {
                // Matching (row,col), sum the value
                auto& tupRes = vecRes[it->second];
                std::get<0>(tupRes) += std::get<0>(tupB);
            } else {
                // No match, append as new
                vecRes.push_back(tupB);
            }
        }
    }

    return result;
}

void run_test_case( const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
                    std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& A_diag,
                    std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& B_diag,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& results,
                    std::ofstream& out, std::ofstream& Energyout) {
    

    

    
    int COL = A_offsets.size();
    int ROW = B_offsets.size();

    std::vector<DiagonalReduction*> diagonalReductions;
    std::vector<std::vector<int>> reductionMap = generateReductionMap(A_offsets, B_offsets, diagonalReductions, out);
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

    results = combineMaps(grid.getResults(), results);
    grid.printEnergy(Energyout);
    out << "Total Cycles: " << cycle << "\n";



    out << "========================================\n";

    //delete
    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    //for (auto* conn : bottom_out) delete conn;
}

int main() {
    int total_unsuccessful = 0;
    int total_cases = 0;
    int size = 256;

    std::cout << "Starting tests for symmetric offsets...\n";
    std::ofstream out("outputs/output_size_" + std::to_string(size) + "_Blocked.txt");
    std::ofstream Energyout("outputs/output_size_" + std::to_string(size) + "_Blocked.power");
    
    std::string filenameA = "./outputs/8/matrix_output_1.txt";
    std::string filenameB = "./outputs/8/matrix_output_2.txt";
    const std::vector<int>& A_offsets = extractDiagonalOffsets(filenameA);
    const std::vector<int>& B_offsets = extractDiagonalOffsets(filenameB);

    std::unordered_map<int, std::vector<std::tuple<double, int, int>>>  A_diag = createDiagonalMap(filenameA, A_offsets, size);
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>>  B_diag = createDiagonalMap(filenameB, B_offsets, size);

    std::tuple<
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>,
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>,
    std::vector<int>, 
    std::vector<int>  
    > split_diagonals = split_double_diagonals_by_size(A_diag, B_diag, 256);
    //print the split diagonals
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = std::get<0>(split_diagonals);
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> B_diag_groups = std::get<1>(split_diagonals);

    std::map<int, std::vector<std::tuple<double, int, int>>> results;
    for (int i = 0; i < A_diag_groups.size(); ++i) {
        std::vector<int> A_offsets = rebuildOffsets(A_diag_groups[i]);
        std::vector<int> B_offsets = rebuildOffsets(B_diag_groups[i]); 
        run_test_case(A_offsets, B_offsets, A_diag_groups[i], B_diag_groups[i], results, out, Energyout);
    }
    
    std::map<int, std::vector<double>> revised_results = addMissingZeros(results, size);
    for (const auto& [key, vec] : revised_results) {
        out << "Diagonal " << key << ": ";
        for (double val : vec) {
            out << val << " ";
        }
        out << "\n";
    }
   
    std::cout << "Total test cases: " << total_cases << "\n";
    std::cout << "Success: " << total_cases - total_unsuccessful << "\n";
    std::cout << "Successful Rate: " << (100.0 * (total_cases - total_unsuccessful) / total_cases) << "%" << "(" << total_cases - total_unsuccessful << "/" << total_cases << ")\n";

    return 0;
}
