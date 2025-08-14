#include "./include/Grid.h"
#include "./include/Connection.h"
#include "./include/TreeReducer.h"
#include "./include/Utility.h"
#include "./include/Cache.h"
#include "./include/DiagonalReduction.h"

#include <iostream>
#include <vector>
#include <fstream>
#include <mutex>
#include <future>
#include <sstream>
#include <algorithm>
//#define SINGLE



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

std::unordered_map<int, int>
createOffsetToGroupMap(
    const std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>& groups
) {
    std::unordered_map<int, int> offsetToGroup;

    for (const auto& [groupIdx, offsetsMap] : groups) {
        for (const auto& [offset, vec] : offsetsMap) {
            // Insert only if not already present
            offsetToGroup.emplace(offset, groupIdx);
        }
    }

    return offsetToGroup;
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

int run_test_case( const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& A_diag,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& B_diag,
                    Scheduler& scheduler, int C_base, std::unordered_map<int, int> C_offset_to_group,
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

    
    GroupData result = grid.getResults();
    //print C_offset_to_group
    // std::cout << "C_offset_to_group:\n";
    // for (const auto& [offset, group] : C_offset_to_group) {
    //     std::cout << "Offset: " << offset << ", Group: " << group << "\n";
    // }
    for (const auto& [offset, entries] : result) {
        // std::cout << "Processing offset: " << offset << "\n";
        // std::cout << "Fetch group index: " << C_offset_to_group.at(offset) + C_base<< "\n";
        GroupData groupData = scheduler.requestGroup(C_offset_to_group.at(offset) + C_base);
        for (const auto& [value, i, j] : entries) {
            for (auto& [val, row, col] : groupData[offset]) {
                if (row == i && col == j) {
                    val += value;  // Sum the values
                    break;
                }
            }
        }
        // for (const auto& [val, row, col] : groupData[offset]) {
        //     std::cout << "C[" << row << "][" << col << "] = " << val << "\n";
        // }
        scheduler.storeGroup(C_offset_to_group.at(offset) + C_base, groupData);
    }
    grid.printEnergy(Energyout);
    out << "Total Cycles: " << cycle << "\n";



    out << "========================================\n";

    //delete
    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    //for (auto* conn : bottom_out) delete conn;
    return cycle;
}

int main(int argc, char* argv[]) {
    int total_unsuccessful = 0;
    int total_cases = 0;
    // int qubit_size = argc > 1 ? std::stoi(argv[1]) : 10; // Default to 10 if no argument is provided
    // //int cache_size = argc > 2 ? std::stoi(argv[2]) : 64;
    int total_cycles = 0;
    // int grid_row = argc > 2 ? std::stoi(argv[2]) : 3;
    // int grid_col = argc > 3 ? std::stoi(argv[3]) : 8;
    // int cache_set = argc > 4 ? std::stoi(argv[4]) : 2; // Default to 2 if no argument is provided
    // int cache_way = argc > 5 ? std::stoi(argv[5]) : 2; // Default to 2 if no argument is provided
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        size_t eq_pos = arg.find('=');
        if (arg.rfind("-", 0) == 0 && eq_pos != std::string::npos) {
            std::string key = arg.substr(1, eq_pos - 1);
            std::string value = arg.substr(eq_pos + 1);
            args[key] = value;
        }
    }

    // Extract values with defaults
    int qubit_size = args.count("qubit") ? std::stoi(args["qubit"]) : 10;
    //std::string filename = args.count("file") ? args["file"] : "";
    int size = static_cast<int>(std::pow(2, qubit_size));
    int grid_row = args.count("row") ? std::stoi(args["row"]) : 3;
    int grid_col = args.count("col") ? std::stoi(args["col"]) : 8;
    int cache_set = args.count("set") ? std::stoi(args["set"]) : 2;
    int cache_way = args.count("way") ? std::stoi(args["way"]) : 2;
    //int iterations = args.count("iter") ? std::stoi(args["iter"]) : 1;
    //std::string folder = args.count("folder") ? args["folder"] : "";
    DRAMStorage dram;
    SetAssociativeCache cache(cache_set, cache_way);

    // Create TwoLevelBuffer
    TwoLevelBuffer buffer(dram, cache);

    // Scheduler
    Scheduler scheduler(buffer);


    #ifdef SINGLE
    std::cout << "Starting tests for symmetric offsets...\n";
    int indexA = argc > 4 ? std::stoi(argv[3]) : 1;
    int indexB = argc > 5 ? std::stoi(argv[4]) : 2;
    std::ofstream out("outputs/" + std::to_string(qubit_size) +"/output_size_cacheSize_" + std::to_string(cache_size)+ "_" + std::to_string(indexA) + "_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_cache" + std::to_string(cache_set) + std::to_string(cache_way) + "_DBlocked.log");
    std::ofstream Energyout("outputs/" + std::to_string(qubit_size) +"/output_size_cacheSize_" + std::to_string(cache_size)+ "_" + std::to_string(indexA) + "_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_cache" + std::to_string(cache_set) + std::to_string(cache_way) + "_DBlocked.count");
    std::string filenameA = "./outputs/" + std::to_string(qubit_size) + "/matrix_output_" + std::to_string(indexA) + ".txt";
    std::string filenameB = "./outputs/" + std::to_string(qubit_size) + "/matrix_output_" + std::to_string(indexB) + ".txt";
    const std::vector<int>& A_offsets = extractDiagonalOffsets(filenameA);
    const std::vector<int>& B_offsets = extractDiagonalOffsets(filenameB);

    std::unordered_map<int, std::vector<std::tuple<double, int, int>>>  A_diag = createDiagonalMap(filenameA, A_offsets, size);
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>>  B_diag = createDiagonalMap(filenameB, B_offsets, size);

    int num_A = std::ceil(A_diag.size() / static_cast<double>(grid_col));
        int num_B = std::ceil(B_diag.size() / static_cast<double>(grid_row));

        std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = splitDiagonals(A_diag, num_A);
        std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> B_diag_groups = splitDiagonals(B_diag, num_B);

    
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
        // Convert unordered_map to GroupData (map)
        GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
        dram.store(groupIndex, groupData);
    }
    int dram_size = dram.size();
    for (const auto& [groupIndex, groupDataRaw] : B_diag_groups) {
        // Convert unordered_map to GroupData (map)
        GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
        dram.store(groupIndex + dram_size, groupData);
    }
    

    std::map<int, std::vector<std::tuple<double, int, int>>> results;
    for (int i = 0; i < A_diag_groups.size(); ++i) {
        for (int j = 0; j < B_diag_groups.size(); j++){
            std::map<int, std::vector<std::tuple<double, int, int>>> A_diag = scheduler.requestGroup(i);
            std::map<int, std::vector<std::tuple<double, int, int>>> B_diag = scheduler.requestGroup(j + dram_size);
            std::vector<int> A_offsets = rebuildOffsets(A_diag);
            std::vector<int> B_offsets = rebuildOffsets(B_diag);
            total_cycles += run_test_case(A_offsets, B_offsets, A_diag, B_diag, results, out, Energyout);
        }
    }
    
    std::map<int, std::vector<double>> revised_results = addMissingZeros(results, size);
    std::string output_filename = "outputs/output_size_" + std::to_string(qubit_size) + "_cacheSize_" + std::to_string(cache_size)+ "_"  + std::to_string(indexA) + "x" + std::to_string(indexB) + "_DBlocked_result.txt";
    saveDiagonalMatrixDense(revised_results, size, output_filename);
    buffer.showCacheStats();
    std::cout << "Total cycles: " << total_cycles << "\n";
    compareMatrices("./outputs/output_size_10_cacheSize_64_1x2_Blocked_result.txt", output_filename, size);
    #else

    std::cout << "Starting chained multiplication of " << qubit_size << " matrices.\n";

    std::ofstream out("/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/output_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_DBlocked.log");
    std::ofstream Energyout("/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/output_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_DBlocked.power");

    std::string basePath = "/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/data/";

    // Load the first matrix
    std::string filenameA = basePath + "matrix_output_1.txt";
    std::vector<int> A_offsets = extractDiagonalOffsets(filenameA);
    auto current_diag = createDiagonalMap(filenameA, A_offsets, size);
    //std::map<int, std::vector<double>> results;

    
    //Occupy the DRAM first 1000 entries as result storage
    //Assume each matrix has at most 1000 entries
    const int result_index_start = 0;
    const int result_index_end = 1000;

    const int maxA = 1000; // Adjust as needed
    const int maxB = 1000; // Adjust as needed
    //const int maxc = 1000;
    const int stride = maxA + maxB;

    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = splitDiagonals(current_diag, grid_col);
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            dram.initialStore(result_index_end + 1 + groupIndex, groupData);
    }
    int size_A = A_diag_groups.size();
    std::vector<int> C_offsets;
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> C_diag_groups;
    int C_base;
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> results;
    for (int k = 2; k <= qubit_size; ++k) {
        std::cout << "Multiplying matrix_output_" << (k-1) << ".txt by matrix_output_" << k << ".txt\n";
        
         // Load next B
        std::string filenameB = basePath + "matrix_output_" + std::to_string(k) + ".txt";
        std::vector<int> B_offsets = extractDiagonalOffsets(filenameB);
        auto next_diag = createDiagonalMap(filenameB, B_offsets, size);
        auto B_diag_groups = splitDiagonals(next_diag, grid_row);

        // Initialize C
        if (k == 2) {
            C_offsets = computeResultDiagonals(A_offsets, B_offsets, size);
        } else {
            C_offsets = computeResultDiagonals(C_offsets, B_offsets, size);
        }
        //print C_offsets
        // for (const auto& offset : C_offsets) {
        //     std::cout << offset << " ";
        // }
        // std::cout << "\n";
        auto C_diag = initializeCdiagGroups(C_offsets, size);
        C_diag_groups = splitDiagonals(C_diag, grid_col);
        std::unordered_map<int, int> C_offset_to_group = createOffsetToGroupMap(C_diag_groups);

        // Compute indices
        int baseIndex = result_index_end + 1 + k * stride;

        int A_base;
        if (k == 2) {
            A_base = result_index_end + 1;
        } else {
            A_base = result_index_end + 1 + (k - 1) * stride + maxA + maxB;
        }
        int B_base = baseIndex + maxA;
        C_base = baseIndex + maxA + maxB;

        // Store B
        for (const auto& [groupIndex, groupDataRaw] : B_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            dram.initialStore(B_base + groupIndex, groupData);
        }

        // Store C
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            dram.initialStore(C_base + groupIndex, groupData);
        }

        // std::cout << "Current DRAM storage:\n";
        // auto storage = dram.getStorage();
        // for (const auto& [groupIndex, groupData] : storage) {
        //     std::cout << "Group " << groupIndex << ": \n";
        //     for (const auto& [offset, entries] : groupData) {
        //         std::cout << "Offset " << offset << "\n";
        //     }
        // }

        // Prepare accumulator
        std::map<int, std::vector<std::tuple<double, int, int>>> step_result;
        //Print DRAM 
        
        // Compute all combinations
        for (int i = 0; i < size_A; ++i) {
            for (int j = 0; j < B_diag_groups.size(); ++j) {
                //std::cout << "Processing A group " << A_base + i << " and B group " << B_base + j << "\n";
                auto A_diag = scheduler.requestGroup(A_base + i);
                auto B_diag = scheduler.requestGroup(B_base + j);

                auto A_offsets_local = rebuildOffsets(A_diag);
                auto B_offsets_local = rebuildOffsets(B_diag);

                total_cycles += run_test_case(
                    A_offsets_local,
                    B_offsets_local,
                    A_diag,
                    B_diag,
                    scheduler,
                    C_base,
                    C_offset_to_group,
                    out,
                    Energyout
                );
            }
        }
        // std::cout << "Current DRAM storage:\n";
        // storage = dram.getStorage();
        // for (const auto& [groupIndex, groupData] : storage) {
        //     std::cout << "Group " << groupIndex << ": \n";
        //     for (const auto& [offset, entries] : groupData) {
        //         std::cout << "Offset " << offset << "\n";
        //     }
        // }
        
        C_offsets.clear();
        results.clear();
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData = dram.showload(C_base + groupIndex);
            dram.erase(C_base + groupIndex); // Clear the group from DRAM
            for (const auto& [offset, entries] : groupData) {
                // Check if all entries are zero
                bool allZero = true;
                for (const auto& [value, i, j] : entries) {
                    if (value != 0.0) {
                        allZero = false;
                        break;
                    }
                }

                if (!allZero) {
                    // Insert into results
                    results[offset] = entries;
                    C_offsets.push_back(offset);
                }
            }
        }
        C_diag_groups = splitDiagonals(results, grid_col);
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            buffer.put(C_base + groupIndex, groupData);
        }
        size_A = C_diag_groups.size();
        std::cout << "Offset size" << k << ": " << C_offsets.size() << "\n";
        std::cout << "Offset:";
        for (const auto& offset : C_offsets) {
            std::cout << offset << " ";
        }
        std::cout << "\n";
        std::cout << "Matrix diagonal size: " << C_offsets.size() << "\n";
        std::cout << "Diagonal";
        for (const auto& offset : C_offsets) {
            std::cout << offset << " ";
        }
        std::cout << "\n";
        //print C_offsets
        // std::cout << "C_offsets size after step " << k << ": " << C_offsets.size() << "\n";
        // std::cout << "C_offsets after step " << k << ": ";
        // for (const auto& offset : C_offsets) {
        //     std::cout << offset << " ";
        // }
        // std::cout << "\n";
        
        //print DRAM storage
        // std::cout << "Current DRAM storage:\n";
        // storage = dram.getStorage();
        // for (const auto& [groupIndex, groupData] : storage) {
        //     std::cout << "Group " << groupIndex << ": \n";
        //     for (const auto& [offset, entries] : groupData) {
        //         std::cout << "Offset " << offset << "\n";
        //     }
        // }
        //Print Cache stats after each multiplication
        cache.printStats();
        // For next iteration, the current result becomes this step's result
        //results = addMissingZeros(step_result, size);
        //std::string output_filename_partial = "outputs/output_size_" + std::to_string(qubit_size) + "_DBlocked_result_" + std::to_string(k) + ".txt";
        //saveDiagonalMatrixDense(results, size, output_filename_partial);
        //bool res = compareMatrices("./outputs/intermediate_ghz_result_10_step_" + std::to_string(k) + ".txt", output_filename_partial, size);
        // if (!res) {
        //     std::cerr << "The result of step " << k << " does not match the CPU output.\n";
        //     return 0;
        // }
        //current_diag = convertDiagonalMap(results, size);
        std::cout << "Finished multiplication with matrix_output_" << k << ".txt\n";
        std::cout << "------------------------------------------"<< std::endl;
    }

    // // Optionally: Save the final matrix
    //std::string output_filename = "outputs/output_size_" + std::to_string(qubit_size) + "_DBlocked_result.txt";
    //saveDiagonalMatrixDense(results, size, output_filename);
    std::pair<int, int> hit = buffer.showCacheStats();
    out << "Total cycles: " << total_cycles << "\n";
    out << "Cache Hits: " << hit.first << ", Cache Misses: " << hit.second << "\n";
    std::cout << "Finished.\n";
    std::cout << "Total cycles: " << total_cycles << "\n";
    size_t mem_cycles = buffer.getTotalCycles();
    std::cout << "Mem Cycles: " << mem_cycles << "\n";
    out << "Mem Cycles:" << mem_cycles << "\n";
    //print configuration
    std::cout << "Configuration:\n";
    std::cout << "Qubit Size: " << qubit_size << "\n";
    std::cout << "Grid Size: " << grid_row << "x" << grid_col << "\n";
    std::cout << "Cache Set: " << cache_set << "\n";
    std::cout << "Cache Way: " << cache_way << "\n";
    //bool isEqual = compareMatrices("./outputs/final_ghz_result_" + std::to_string(qubit_size) + ".txt", output_filename, size);
    //bool isEqual = compareMatrices("./outputs/intermediate_ghz_result_10_step_3.txt", output_filename, size);
    //if (isEqual) {
    //    std::cout << "The final result matches the CPU output.\n";
    //} else {
    //    std::cout << "The final result does not match the CPU output.\n";
    //}
    #endif
    return 0;
}
