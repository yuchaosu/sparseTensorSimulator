#include "../include/Cache.h"
#include <cassert>
#include <iostream>
#include "../include/Utility.h"

// Helper function to pretty-print a GroupData for verification
void printGroup(const GroupData& group) {
    for (const auto& [offset, entries] : group) {
        std::cout << "  Offset " << offset << ":\n";
        for (const auto& [val, row, col] : entries) {
            std::cout << "    (" << val << ", " << row << ", " << col << ")\n";
        }
    }
}

void runGroupBufferUnitTest(
    const std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>& A_diag_groups)
{
    std::cout << "Running TwoLevelBuffer group test...\n";

    // Create DRAM storage and load all groups
    DRAMStorage dram;
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
        // Convert unordered_map to GroupData (map)
        GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
        dram.store(groupIndex, groupData);
    }

    // Create a 2-set, 2-way associative cache
    SetAssociativeCache cache(2, 2);

    // Create TwoLevelBuffer
    TwoLevelBuffer buffer(dram, cache);

    // Scheduler
    Scheduler scheduler(buffer);

    // Iterate over groups and request each
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
        std::cout << "\nRequesting group " << groupIndex << ":\n";

        const GroupData& retrieved = scheduler.requestGroup(groupIndex);

        // Print retrieved data
        printGroup(retrieved);

        // Verify the retrieved data matches the original
        GroupData expected(groupDataRaw.begin(), groupDataRaw.end());
        assert(retrieved == expected);
    }

    // Request first group again to see if it is cached
    if (!A_diag_groups.empty()) {
        int firstGroupIndex = A_diag_groups.begin()->first;
        std::cout << "\nRe-requesting group " << firstGroupIndex << " to test cache hit:\n";
        const GroupData& again = scheduler.requestGroup(firstGroupIndex);
        assert(again == GroupData(A_diag_groups.begin()->second.begin(), A_diag_groups.begin()->second.end()));
    }

    // Show cache stats
    buffer.showCacheStats();

    std::cout << "\nAll assertions passed.\n";
}

int main()
{
    int size = 256; // Size of the matrix
    
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



    runGroupBufferUnitTest(A_diag_groups);

    return 0;
}
