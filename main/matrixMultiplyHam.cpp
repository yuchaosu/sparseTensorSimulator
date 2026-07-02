#include "../include/Grid.h"
#include "../include/Connection.h"
#include "../include/TreeReducer.h"
#include "../include/Utility.h"
#include "../include/Cache.h"
#include "../include/DiagonalReduction.h"

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

    grid.cycle(cycle);
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
    // std::string filename = argv[2];
    // int size = pow(2, qubit_size);
    // //int cache_size = argc > 2 ? std::stoi(argv[2]) : 64;
    int total_cycles = 0;
    // int grid_row = argc > 3 ? std::stoi(argv[3]) : 3;
    // int grid_col = argc > 4 ? std::stoi(argv[4]) : 8;
    // int cache_set = argc > 5 ? std::stoi(argv[5]) : 2; // Default to 2 if no argument is provided
    // int cache_way = argc > 6 ? std::stoi(argv[6]) : 2; // Default to 2 if no argument is provided
    // int iterations = argc > 7 ? std::stoi(argv[7]) : 1; // Default to 1 if no argument is provided
    std::map<std::string, std::string> args;

    // Parse arguments of form -key=value
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
    std::string filename = args.count("file") ? args["file"] : "";
    int size = static_cast<int>(std::pow(2, qubit_size));
    int grid_row = args.count("row") ? std::stoi(args["row"]) : 3;
    int grid_col = args.count("col") ? std::stoi(args["col"]) : 8;
    int cache_set = args.count("set") ? std::stoi(args["set"]) : 2;
    int cache_way = args.count("way") ? std::stoi(args["way"]) : 2;
    int iterations = args.count("iter") ? std::stoi(args["iter"]) : 1;
    std::string folder = args.count("folder") ? args["folder"] : "";

    DRAMStorage dram;
    SetAssociativeCache cache(cache_set, cache_way);

    // Create TwoLevelBuffer
    TwoLevelBuffer buffer(dram, cache);

    // Scheduler
    Scheduler scheduler(buffer);

    //std::cout << "Starting chained multiplication of " << qubit_size << " matrices.\n";
    std::string output_name = filename;
    size_t dot_pos = output_name.find_last_of(".");
    if (dot_pos != std::string::npos) {
        output_name = output_name.substr(0, dot_pos);
    }
    // std::ofstream out("/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/output_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_DBlocked.log");
    // std::ofstream Energyout("/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/output_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_DBlocked.power");
 
    std::cout << "filename: " << folder + filename << "\n";
    std::string basePath = folder;

    std::ofstream out;
    out.setstate(std::ios_base::failbit);
    std::ofstream Energyout(folder + output_name +".power");

    // Load the first matrix
    std::string filenameA = basePath + filename;
    std::vector<int> A_offsets = extractDiagonalOffsets(filenameA);
    // std::cout << "A_offsets: ";
    // for (const auto& offset : A_offsets) {
    //    std::cout << offset << " ";
    // }
    // std::cout << "\n";
    auto current_diag = createDiagonalMap(filenameA, A_offsets, size);
    //std::map<int, std::vector<double>> results;

    
    //Occupy the DRAM first 1000 entries as result storage
    //Assume each matrix has at most 1000 entries
    const int result_index_start = 0;
    const int result_index_end = 1000;

    const int maxA = 1000; // Adjust as needed
    const int maxB = 1000; // Adjust as needed
    //const int maxc = 1000;
    const int stride = maxA;
    auto B_offsets = A_offsets;
    
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = splitDiagonals(current_diag, grid_col);
    auto B_diag_groups = A_diag_groups; // For now, use the same groups for B
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            dram.initialStore(groupIndex, groupData);
    }
    int size_A = A_diag_groups.size();
    int B_base = 0;
    std::vector<int> C_offsets;
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> C_diag_groups;
    int C_base;
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> results;
    int old_size_A = 0;
    int old_A_base = 0;
    for (int k = 0; k < iterations; ++k) {
        std::cout << "Iteration" << k << ":\n";
        
         // Load next B

        // Initialize C
        if (k == 0) {
            std::cout << "A diagonal size:" << A_offsets.size() << "\n";
            C_offsets = computeResultDiagonals(A_offsets, B_offsets, size);
        } else {
            std::cout << "A diagonal size:" << C_offsets.size() << "\n";
            C_offsets = computeResultDiagonals(C_offsets, B_offsets, size);
        }
        auto C_diag = initializeCdiagGroups(C_offsets, size);
        C_diag_groups = splitDiagonals(C_diag, grid_col);
        std::unordered_map<int, int> C_offset_to_group = createOffsetToGroupMap(C_diag_groups);
        //print A offsets
        std::cout << "A offsets: ";
        for (const auto& offset : A_offsets) {
            std::cout << offset << " ";
        }
        std::cout << "\n";
        // Compute indices
        //int baseIndex = 1000;

        int A_base = 1000 * k;
        
        C_base = A_base + 1000;

        // // Store B
        // for (const auto& [groupIndex, groupDataRaw] : B_diag_groups) {
        //     GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
        //     dram.initialStore(B_base + groupIndex, groupData);
        // }

        // Store C
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            dram.initialStore(C_base + groupIndex, groupData);
        }
        //print DRAM storage
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
        int cycles = 0;
        // Compute all combinations
        for (int i = 0; i < size_A; ++i) {
            for (int j = 0; j < B_diag_groups.size(); ++j) {
                //std::cout << "Processing A group " << A_base + i << " and B group " << B_base + j << "\n";
                auto A_diag = scheduler.requestGroup(A_base + i);
                auto B_diag = scheduler.requestGroup(B_base + j);

                auto A_offsets_local = rebuildOffsets(A_diag);
                auto B_offsets_local = rebuildOffsets(B_diag);

                cycles = run_test_case(
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
                total_cycles += cycles;
            }
        }

        for (int i =0; i < old_size_A; ++i) {
            dram.erase(old_A_base + i); // Clear the group from DRAM
        }
        if (k == 1) {
            old_A_base = A_base;
            old_size_A = size_A;
        }
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
        std::cout << "Matrix diagonal size: " << C_offsets.size() << "\n";
        std::cout << "Diagonal";
        for (const auto& offset : C_offsets) {
            std::cout << offset << " ";
        }
        
        std::cout << "\n";
        cache.printStats();
        std::cout << "Cycles: " << cycles << "\n";
        std::cout << "Finished multiplication with matrix_output_" << k << ".txt\n";
        std::cout << "------------------------------------------"<< std::endl;
    }

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

    std::cout << "Statistics saved to " << folder + output_name + ".power" << "\n";
    return 0;
}
