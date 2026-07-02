#include "../include/Grid.h"
#include "../include/Connection.h"
#include "../include/TreeReducer.h"
#include "../include/Utility.h"
#include "../include/DiagonalReduction.h"
#include "../include/HBM.h"
#include "../include/RamulatorHBM.h"
#include "../include/PE.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

using GroupData = std::map<int, std::vector<std::tuple<double, int, int>>>;

namespace {
constexpr uint32_t kNumHBMChannels = 8;
constexpr uint64_t kBurstBytes = 64;
constexpr uint64_t kRowBytes = 2048;
constexpr uint64_t kChannelRowSpan = kRowBytes * kNumHBMChannels;

uint64_t alignTo(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const uint64_t remainder = value % alignment;
    return remainder ? value + (alignment - remainder) : value;
}

uint64_t composeSystemAddress(uint32_t channel, uint64_t channelLocalAddr) {
    const uint64_t stripeOffset = channelLocalAddr % kBurstBytes;
    const uint64_t stripeIndexLocal = channelLocalAddr / kBurstBytes;
    const uint64_t stripeIndexSystem = stripeIndexLocal * kNumHBMChannels + channel;
    return stripeIndexSystem * kBurstBytes + stripeOffset;
}

size_t computeGroupBytes(const GroupData& data) {
    size_t bytes = sizeof(uint32_t); // number of diagonals
    for (const auto& [offset, entries] : data) {
        bytes += sizeof(int);               // offset value
        bytes += sizeof(uint32_t);          // entry count
        bytes += entries.size() * (sizeof(double) + 2 * sizeof(int));
    }
    return std::max<size_t>(bytes, sizeof(uint32_t));
}

size_t bytesToBursts(size_t bytes) {
    return static_cast<size_t>((bytes + kBurstBytes - 1) / kBurstBytes);
}

// ---- Ground-truth verification helpers (small sizes only) ----
using DenseMat = std::vector<std::vector<double>>;

template <typename DiagMap>
void fillDense(DenseMat& M, const DiagMap& diag, int n) {
    for (const auto& [off, entries] : diag) {
        for (const auto& [v, i, j] : entries) {
            if (i >= 0 && i < n && j >= 0 && j < n) M[i][j] = v;
        }
    }
}

DenseMat denseMatMul(const DenseMat& A, const DenseMat& B, int n) {
    DenseMat C(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < n; ++k) {
            const double a = A[i][k];
            if (a == 0.0) continue;
            const double* brow = B[k].data();
            double* crow = C[i].data();
            for (int j = 0; j < n; ++j) crow[j] += a * brow[j];
        }
    }
    return C;
}

// Compare a dense reference against the simulator's C scratchpad. Returns the
// max absolute difference and counts entries that disagree beyond tol.
void compareDense(const DenseMat& ref, const std::map<int, GroupData>& C_scratch,
                  int n, double tol, double& maxAbsDiff, long& mismatches) {
    DenseMat sim(n, std::vector<double>(n, 0.0));
    for (const auto& [gidx, gd] : C_scratch) fillDense(sim, gd, n);
    maxAbsDiff = 0.0;
    mismatches = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double d = std::abs(sim[i][j] - ref[i][j]);
            if (d > maxAbsDiff) maxAbsDiff = d;
            if (d > tol) ++mismatches;
        }
    }
}

struct HBMAllocation {
    uint32_t channel = 0;
    uint64_t channelBase = 0;
    size_t sizeBytes = 0;
    size_t burstCount = 0;
};

class HBMMemory {
public:
    HBMMemory()
        : controller(), channelHeads{}, nextChannel(0), totalCycles(0),
          bytesRead(0), bytesWritten(0), requestId(0) {
        channelHeads.fill(0);
    }

    void initialStore(int groupIndex, const GroupData& data) {
        store(groupIndex, data);
    }

    GroupData load(int groupIndex) {
        auto allocIt = allocations.find(groupIndex);
        if (allocIt == allocations.end()) {
            throw std::runtime_error("HBM load: group missing");
        }
        issueTransfers(allocIt->second, allocIt->second.sizeBytes, false);
        auto storageIt = storage.find(groupIndex);
        if (storageIt == storage.end()) {
            throw std::runtime_error("HBM load: payload missing");
        }
        return storageIt->second;
    }

    void store(int groupIndex, const GroupData& data) {
        size_t sizeBytes = computeGroupBytes(data);
        HBMAllocation& alloc = ensureAllocation(groupIndex, sizeBytes);
        storage[groupIndex] = data;
        alloc.sizeBytes = sizeBytes;
        issueTransfers(alloc, sizeBytes, true);
    }

    void erase(int groupIndex) {
        allocations.erase(groupIndex);
        storage.erase(groupIndex);
    }

    uint64_t getTotalCycles() const {
        return totalCycles;
    }

    void printStats(std::ostream& os) const {
        os << "HBM total simulated time: " << totalCycles << " ns\n";
    }

    // Expose trace control for the internal HBM controller
    void enableTrace(const std::string& path) {
        controller.enableTrace(path);
    }

    void enableTraceAggregate(const std::string& path) {
        controller.enableTraceAggregate(path);
    }

    void disableTraceAggregate() {
        controller.disableTraceAggregate();
    }

    void disableTrace() {
        controller.disableTrace();
    }

    struct Stats {
        uint64_t bytesRead;
        uint64_t bytesWritten;
        uint64_t cycles;
    };

    Stats getStats() const {
        return Stats{bytesRead, bytesWritten, totalCycles};
    }

private:
    HBMAllocation& ensureAllocation(int groupIndex, size_t sizeBytes) {
        const size_t neededBursts = bytesToBursts(sizeBytes);
        auto it = allocations.find(groupIndex);
        uint32_t channel;
        if (it != allocations.end()) {
            if (neededBursts <= it->second.burstCount) {
                it->second.sizeBytes = sizeBytes;
                it->second.burstCount = neededBursts;
                return it->second;
            }
            channel = it->second.channel;
            allocations.erase(it);
        } else {
            channel = nextChannel;
            nextChannel = (nextChannel + 1) % kNumHBMChannels;
        }

        uint64_t& head = channelHeads[channel];
        head = alignTo(head, kRowBytes);
        HBMAllocation alloc;
        alloc.channel = channel;
        alloc.channelBase = head;
        alloc.sizeBytes = sizeBytes;
        alloc.burstCount = neededBursts;
        head += alignTo(alloc.burstCount * kBurstBytes, kRowBytes);

        auto [insertedIt, _] = allocations.emplace(groupIndex, alloc);
        return insertedIt->second;
    }

    void issueTransfers(const HBMAllocation& alloc, size_t sizeBytes, bool isWrite) {
        size_t remaining = sizeBytes;
        for (size_t burst = 0; burst < alloc.burstCount; ++burst) {
            const uint64_t channelAddr = alloc.channelBase + burst * kBurstBytes;
            const uint64_t systemAddr = composeSystemAddress(alloc.channel, channelAddr);
            const size_t chunk = std::min<size_t>(kBurstBytes, remaining);
            queueRequest(systemAddr, chunk, isWrite);
            if (remaining >= chunk) {
                remaining -= chunk;
            } else {
                remaining = 0;
            }
        }
        drain();
    }

    void queueRequest(uint64_t address, size_t sizeBytes, bool isWrite) {
        size_t chunk = std::max<size_t>(sizeBytes, 1);
        bool queued = false;
        while (!queued) {
            try {
                controller.addRequest(address, isWrite, chunk, requestId++);
                queued = true;
            } catch (const std::runtime_error&) {
                controller.tick();
            }
        }
        if (isWrite) {
            bytesWritten += chunk;
        } else {
            bytesRead += chunk;
        }
    }

    void drain() {
        while (controller.hasPendingRequests()) {
            controller.tick();
        }
        totalCycles = std::max<uint64_t>(totalCycles, controller.getMaxChannelTime());
    }

    RamulatorHBM controller;   // SOTA HBM4 via Ramulator 2.1 (was homegrown HBMController)
    std::array<uint64_t, kNumHBMChannels> channelHeads;
    uint32_t nextChannel;
    std::unordered_map<int, HBMAllocation> allocations;
    std::unordered_map<int, GroupData> storage;
    uint64_t totalCycles;
    uint64_t bytesRead;
    uint64_t bytesWritten;
    int requestId;
};

class HBMScheduler {
public:
    explicit HBMScheduler(HBMMemory& memoryRef) : memory(memoryRef) {}

    GroupData requestGroup(int groupIndex) {
        return memory.load(groupIndex);
    }

    void storeGroup(int groupIndex, const GroupData& data) {
        memory.store(groupIndex, data);
    }

    uint64_t getTotalCycles() const {
        return memory.getTotalCycles();
    }

private:
    HBMMemory& memory;
};

} // namespace

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
    std::reverse(B_offsets_reversed.begin(), B_offsets_reversed.end());
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
            offsetToGroup.emplace(offset, groupIdx);
        }
    }

    return offsetToGroup;
}

std::map<int, std::vector<std::tuple<double, int, int>>> combineMaps(const std::map<int, std::vector<std::tuple<double, int, int>>>& mapA, const std::map<int, std::vector<std::tuple<double, int, int>>>& mapB) {
    std::map<int, std::vector<std::tuple<double, int, int>>> result = mapA;

    for (const auto& [key, vecB] : mapB) {
        auto& vecRes = result[key];

        std::map<std::pair<int,int>, size_t> indexMap;
        for (size_t i = 0; i < vecRes.size(); ++i) {
            int row = std::get<1>(vecRes[i]);
            int col = std::get<2>(vecRes[i]);
            indexMap[{row, col}] = i;
        }

        for (const auto& tupB : vecB) {
            int rowB = std::get<1>(tupB);
            int colB = std::get<2>(tupB);
            auto it = indexMap.find({rowB, colB});
            if (it != indexMap.end()) {
                auto& tupRes = vecRes[it->second];
                std::get<0>(tupRes) += std::get<0>(tupB);
            } else {
                vecRes.push_back(tupB);
            }
        }
    }

    return result;
}

int run_test_case( const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& A_diag,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& B_diag,
                    HBMScheduler& scheduler, int C_base, std::unordered_map<int, int> C_offset_to_group,
                    std::map<int, GroupData>& C_scratch,
                    std::ofstream& out, std::ofstream& Energyout) {

    int COL = A_offsets.size();
    int ROW = B_offsets.size();

    std::vector<DiagonalReduction*> diagonalReductions;
    std::vector<std::vector<int>> reductionMap = generateReductionMap(A_offsets, B_offsets, diagonalReductions, out);
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

    std::vector<Connection*> left_in(ROW), top_in(COL);
    for (int i = 0; i < ROW; ++i) {
        left_in[i] = new Connection(out);
    }
    for (int i = 0; i < COL; ++i) {
        top_in[i] = new Connection(out);
    }
    grid.setInputConnections(top_in, left_in);

    std::vector<std::vector<DataPackage>> A_diag_packages = buildDatapackage(A_diag);
    std::vector<std::vector<DataPackage>> B_diag_packages = buildDatapackage(B_diag);
    std::reverse(B_diag_packages.begin(), B_diag_packages.end());
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

    std::vector<int> A_inject_index(COL, 0);
    std::vector<int> B_inject_index(ROW, 0);

    bool injection_done_left = false;
    bool injection_done_top = false;
    bool injection_done = false;
    std::vector<bool> col_finish_sent(COL, false);
    std::vector<bool> row_finish_sent(ROW, false);
    while(true) {
        out << "===== Cycle " << cycle << " =====\n";

        for (int col = 0; col < COL; ++col) {
            if (col < static_cast<int>(A_diag_packages.size()) && cycle >= col) {
                int& idx = A_inject_index[col];
                const auto& vec = A_diag_packages[col];

                if (idx < static_cast<int>(vec.size()) && !top_in[col]->pendingSrc()) {
                    top_in[col]->receiveSrc(vec[idx]);
                    out << "Injecting A diagonal package at column " << col << ": " << vec[idx] << "\n";
                    ++idx;

                    if (idx == static_cast<int>(vec.size())) {
                        top_in[col]->receiveInjectionFinished(true);
                        out << "Injecting per-column finish signal at column " << col << ": true\n";
                    }
                }
            }
        }

        for (int row = 0; row < ROW; ++row) {
            if (row < static_cast<int>(B_diag_packages.size()) && cycle >= row) {
                int& idx = B_inject_index[row];
                const auto& vec = B_diag_packages[row];

                if (idx < static_cast<int>(vec.size()) && !left_in[row]->pendingSrc()) {
                    left_in[row]->receiveSrc(vec[idx]);
                    out << "Injecting B diagonal package at row " << row << ": " << vec[idx] << "\n";
                    ++idx;

                    if (idx == static_cast<int>(vec.size())) {
                        left_in[row]->receiveInjectionFinished(true);
                        out << "Injecting per-row finish signal at row " << row << ": true\n";
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
        if(injection_done && grid.isIdle()) {
            out << "All data injected and processed. Breaking out of cycle loop.\n";
            break;
        }
        ++cycle;
        out << "Left Injection Signal: " << injection_done_left << ", Top Injection Signal: " << injection_done_top << ", Grid Idle: " << grid.isIdle() << "\n";
    }

    // Accumulate this tile's partial results into the on-chip C scratchpad.
    // (Previously each output offset was re-loaded from and stored back to HBM
    // per tile; now C lives in the edge scratchpad and is written to HBM once
    // per group at the end of the iteration.)
    GroupData result = grid.getResults();
    for (const auto& [offset, entries] : result) {
        GroupData& groupData = C_scratch[C_offset_to_group.at(offset) + C_base];
        auto& vec = groupData[offset];
        for (const auto& [value, i, j] : entries) {
            bool found = false;
            for (auto& [val, row, col] : vec) {
                if (row == i && col == j) { val += value; found = true; break; }
            }
            if (!found) vec.emplace_back(value, i, j);
        }
    }
    grid.printEnergy(Energyout);
    out << "Total Cycles: " << cycle << "\n";

    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    return cycle;
}

int main(int argc, char* argv[]) {
    int total_cycles = 0;
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

    int qubit_size = args.count("qubit") ? std::stoi(args["qubit"]) : 10;
    std::string filename = args.count("file") ? args["file"] : "";
    int size = static_cast<int>(std::pow(2, qubit_size));
    int grid_row = args.count("row") ? std::stoi(args["row"]) : 3;
    int grid_col = args.count("col") ? std::stoi(args["col"]) : 8;
    int iterations = args.count("iter") ? std::stoi(args["iter"]) : 1;
    std::string folder = args.count("folder") ? args["folder"] : "";
    // Ground-truth verification: compare each iteration's C against a dense
    // matrix power. Only enabled on small matrices (O(size^3) per iteration).
    bool verify = args.count("verify") ? std::stoi(args["verify"]) != 0 : false;
    const int kVerifyMaxSize = 4096;  // qubit <= 12

    HBMMemory hbmMemory;
    HBMScheduler scheduler(hbmMemory);

    std::string output_name = filename;
    size_t dot_pos = output_name.find_last_of(".");
    if (dot_pos != std::string::npos) {
        output_name = output_name.substr(0, dot_pos);
    }
    folder = "/mnt/beegfs/ysu34/hamlib/" + folder;
    std::cout << "filename: " << folder + filename << "\n";
    std::string basePath = folder;

    // Enable tracing: HBM transfer trace and PE compute trace (use output_name + folder)
    std::string hbm_trace_path = folder + output_name + ".hbm_trace.csv";
    std::string compute_trace_path = folder + output_name + ".compute_trace.csv";
    try {
        // Use aggregate tracing to avoid huge per-operation CSVs
        hbmMemory.enableTraceAggregate(hbm_trace_path + ".aggregate.csv");
    } catch (...) {
        std::cerr << "Warning: failed to enable HBM aggregate trace at " << hbm_trace_path << ".aggregate.csv\n";
    }
    PE::enableComputeTraceAggregate(compute_trace_path + ".aggregate.csv");

    std::ofstream out;
    out.setstate(std::ios_base::failbit);
    std::ofstream Energyout(folder + output_name +".power");

    std::string filenameA = basePath + filename;
    std::vector<int> A_offsets = extractDiagonalOffsets(filenameA);
    auto current_diag = createDiagonalMap(filenameA, A_offsets, size);

    // Ground-truth reference: dense H and its running power. The driver computes
    // C_k = H^{k+2}, so ref starts at H and is multiplied by H each iteration.
    DenseMat denseH, ref;
    if (verify) {
        if (size > kVerifyMaxSize) {
            std::cout << "[verify] disabled: size " << size << " > " << kVerifyMaxSize
                      << " (O(size^3) too large)\n";
            verify = false;
        } else {
            denseH.assign(size, std::vector<double>(size, 0.0));
            fillDense(denseH, current_diag, size);
            ref = denseH;  // H^1
        }
    }

    const int maxA = 1000;
    const int maxB = 1000;
    const int stride = maxA;
    (void)maxA;
    (void)maxB;
    (void)stride;
    auto B_offsets = A_offsets;

    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = splitDiagonals(current_diag, grid_col);
    auto B_diag_groups = A_diag_groups;
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
        GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
        hbmMemory.initialStore(groupIndex, groupData);
    }
    int size_A = static_cast<int>(A_diag_groups.size());
    int B_base = 0;
    std::vector<int> C_offsets;
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> C_diag_groups;
    int C_base;
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> results;
    int old_size_A = 0;
    int old_A_base = 0;
    HBMMemory::Stats prevStats = hbmMemory.getStats();

    // ---- On-chip edge scratchpad model ----
    // Fixed 2 MB SRAM at the array edges: A/B operand tiles are staged from HBM
    // (double-buffered / ping-pong so the next tile's load overlaps the current
    // tile's compute), and C is accumulated on-chip and written back to HBM once
    // per group. kAccelClockGHz converts grid cycles to time (1 GHz => 1 ns/cyc).
    constexpr size_t kScratchpadBytes = 2 * 1024 * 1024;
    constexpr double kAccelClockGHz = 1.0;
    constexpr double kZeroTol = 1e-12;   // below this, a C entry is treated as zero
    double total_compute_ns = 0.0;   // array-busy time (sum of tile computes)
    double total_hidden_ns = 0.0;    // DRAM load hidden behind compute via prefetch
    size_t scratchpad_peak_bytes = 0;
    bool scratchpad_overflow = false;

    for (int k = 0; k < iterations; ++k) {
        std::cout << "Iteration" << k << ":\n";

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
        std::cout << "A offsets: ";
        for (const auto& offset : A_offsets) {
            std::cout << offset << " ";
        }
        std::cout << "\n";

        int A_base = 1000 * k;

        C_base = A_base + 1000;

        // C is accumulated in the on-chip scratchpad, initialised (zeros) from
        // the split C diagonals — no per-iteration HBM pre-store of C.
        std::map<int, GroupData> C_scratch;
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            C_scratch[C_base + groupIndex] = GroupData(groupDataRaw.begin(), groupDataRaw.end());
        }

        size_t maxA_bytes = 0, maxB_bytes = 0;  // largest A/B tile (sizes the buffers)

        // Operand reuse (tiling): B is invariant across the outer loop, so load
        // all B groups from HBM once and keep them resident in the scratchpad
        // (was reloaded for every (i,j)). A_i is loaded once per outer row and
        // reused across all j (was reloaded for every j). This removes the
        // O(size_A * B_groups) redundant operand traffic. Result is unchanged;
        // only scheduling/HBM traffic differs.
        std::map<int, GroupData> B_resident;
        size_t B_resident_bytes = 0;
        for (int j = 0; j < static_cast<int>(B_diag_groups.size()); ++j) {
            B_resident[j] = scheduler.requestGroup(B_base + j);   // one-time HBM load
            size_t bb = computeGroupBytes(B_resident[j]);
            B_resident_bytes += bb;
            maxB_bytes = std::max(maxB_bytes, bb);
        }

        double prev_row_compute_ns = 0.0;
        bool first_row = true;

        int cycles = 0;
        for (int i = 0; i < size_A; ++i) {
            // Prefetch A_i (double-buffered): its load overlaps the previous
            // row's compute.
            uint64_t dramBefore = hbmMemory.getStats().cycles;
            auto A_diag_local = scheduler.requestGroup(A_base + i);
            double A_load_ns = static_cast<double>(hbmMemory.getStats().cycles - dramBefore);
            if (!first_row) total_hidden_ns += std::min(prev_row_compute_ns, A_load_ns);
            first_row = false;

            maxA_bytes = std::max(maxA_bytes, computeGroupBytes(A_diag_local));
            auto A_offsets_local = rebuildOffsets(A_diag_local);

            double row_compute_ns = 0.0;
            for (int j = 0; j < static_cast<int>(B_diag_groups.size()); ++j) {
                const auto& B_diag_local = B_resident[j];       // resident, no HBM load
                auto B_offsets_local = rebuildOffsets(B_diag_local);

                auto A_diag_copy = A_diag_local;                // run_test_case takes non-const refs
                auto B_diag_copy = B_diag_local;
                cycles = run_test_case(
                    A_offsets_local,
                    B_offsets_local,
                    A_diag_copy,
                    B_diag_copy,
                    scheduler,
                    C_base,
                    C_offset_to_group,
                    C_scratch,
                    out,
                    Energyout
                );
                total_cycles += cycles;
                row_compute_ns += static_cast<double>(cycles) / kAccelClockGHz;
            }
            total_compute_ns += row_compute_ns;
            prev_row_compute_ns = row_compute_ns;
        }

        // Scratchpad occupancy: A double buffer (ping-pong => 2x) + resident B
        // + resident C accumulation region.
        (void)maxB_bytes;
        size_t cBytes = 0;
        for (const auto& [gidx, gd] : C_scratch) cBytes += computeGroupBytes(gd);
        size_t sp_bytes = 2 * maxA_bytes + B_resident_bytes + cBytes;
        scratchpad_peak_bytes = std::max(scratchpad_peak_bytes, sp_bytes);
        if (sp_bytes > kScratchpadBytes) scratchpad_overflow = true;

        for (int i =0; i < old_size_A; ++i) {
            hbmMemory.erase(old_A_base + i);
        }
        if (k == 1) {
            old_A_base = A_base;
            old_size_A = size_A;
        }
        C_offsets.clear();
        results.clear();
        // Read the finalised C straight from the on-chip scratchpad (no HBM load).
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            const GroupData& groupData = C_scratch.at(C_base + groupIndex);
            for (const auto& [offset, entries] : groupData) {
                // Tolerance-based zero test: treat |v| <= kZeroTol as zero so the
                // surviving-diagonal set is stable under FP accumulation order.
                // kZeroTol is far below physical Hamiltonian values.
                bool allZero = true;
                for (const auto& [value, i, j] : entries) {
                    if (std::abs(value) > kZeroTol) {
                        allZero = false;
                        break;
                    }
                }

                if (!allZero) {
                    results[offset] = entries;
                    C_offsets.push_back(offset);
                }
            }
        }

        // Ground-truth check: this iteration's C must equal H^{k+2}.
        if (verify) {
            ref = denseMatMul(ref, denseH, size);
            double maxAbsDiff = 0.0;
            long mismatches = 0;
            compareDense(ref, C_scratch, size, 1e-6, maxAbsDiff, mismatches);
            std::cout << "[verify] iter " << k << ": "
                      << (mismatches == 0 ? "PASS" : "FAIL")
                      << " (max_abs_diff=" << maxAbsDiff
                      << ", mismatches=" << mismatches << ")\n";
        }

        C_diag_groups = splitDiagonals(results, grid_col);
        // Single write-back of the final C groups to HBM (persist for next iter).
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            scheduler.storeGroup(C_base + groupIndex, groupData);
        }
        size_A = static_cast<int>(C_diag_groups.size());
        std::cout << "Matrix diagonal size: " << C_offsets.size() << "\n";
        std::cout << "Diagonal";
        for (const auto& offset : C_offsets) {
            std::cout << offset << " ";
        }

        std::cout << "\n";
        HBMMemory::Stats currentStats = hbmMemory.getStats();
        uint64_t deltaCycles = currentStats.cycles - prevStats.cycles;
        uint64_t deltaRead = currentStats.bytesRead - prevStats.bytesRead;
        uint64_t deltaWrite = currentStats.bytesWritten - prevStats.bytesWritten;
        uint64_t deltaBytes = deltaRead + deltaWrite;
        double iterationBandwidth = 0.0;
        if (deltaCycles > 0) {
            iterationBandwidth = static_cast<double>(deltaBytes) * 1e9 /
                                 (static_cast<double>(deltaCycles) * 1024.0 * 1024 * 1024);
        }

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "HBM iteration stats: Read " << (deltaRead / (1024.0 * 1024))
                  << " MiB, Write " << (deltaWrite / (1024.0 * 1024))
                  << " MiB, Bandwidth " << iterationBandwidth << " GiB/s\n";
        std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
        std::cout << "Cycles: " << cycles << "\n";
        std::cout << "Finished multiplication with matrix_output_" << k << ".txt\n";
        std::cout << "------------------------------------------"<< std::endl;

        prevStats = currentStats;
    }

    out << "Total cycles: " << total_cycles << "\n";
    HBMMemory::Stats totalStats = hbmMemory.getStats();
    out << "HBM total time: " << totalStats.cycles << " ns\n";
    out << "HBM bytes read: " << totalStats.bytesRead << ", bytes written: "
        << totalStats.bytesWritten << "\n";
    std::cout << "Finished.\n";
    std::cout << "Total cycles: " << total_cycles << "\n";
    uint64_t mem_cycles = totalStats.cycles;
    std::cout << "HBM Time: " << mem_cycles << " ns\n";
    double totalBandwidth = 0.0;
    if (mem_cycles > 0) {
        uint64_t totalBytes = totalStats.bytesRead + totalStats.bytesWritten;
        totalBandwidth = static_cast<double>(totalBytes) * 1e9 /
                         (static_cast<double>(mem_cycles) * 1024.0 * 1024 * 1024);
    }
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "HBM total transferred: Read " << (totalStats.bytesRead / (1024.0 * 1024))
              << " MiB, Write " << (totalStats.bytesWritten / (1024.0 * 1024))
              << " MiB, Bandwidth " << totalBandwidth << " GiB/s\n";
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    out << "HBM Time:" << mem_cycles << "\n";
    out << "HBM Bytes Read:" << totalStats.bytesRead << ", HB Bytes Written:" << totalStats.bytesWritten << "\n";
    // ---- Memory-latency fraction of the whole process ----
    // With the double-buffered edge scratchpad, each tile's HBM load overlaps the
    // previous tile's compute: total_hidden_ns is the DRAM time hidden that way,
    // so exposed DRAM = total HBM time - hidden. Runtime = compute + exposed.
    const double compute_time_ns = total_compute_ns;
    const double dram_time_ns = static_cast<double>(mem_cycles);
    const double hidden_ns = std::min(total_hidden_ns, dram_time_ns);
    const double exposed_dram_ns = dram_time_ns - hidden_ns;
    const double total_time_ns = compute_time_ns + exposed_dram_ns;
    const double mem_latency_pct =
        total_time_ns > 0.0 ? 100.0 * exposed_dram_ns / total_time_ns : 0.0;
    std::cout << std::fixed << std::setprecision(9);
    std::cout << "Compute time: " << compute_time_ns / 1e9 << " s ("
              << total_cycles << " cycles @ " << kAccelClockGHz << " GHz)\n";
    std::cout << "DRAM (HBM4) time: " << dram_time_ns / 1e9 << " s total, "
              << hidden_ns / 1e9 << " s hidden by prefetch, "
              << exposed_dram_ns / 1e9 << " s exposed\n";
    std::cout << "Total runtime (overlapped): " << total_time_ns / 1e9 << " s\n";
    std::cout << std::setprecision(2);
    std::cout << "Memory-latency percentage: " << mem_latency_pct << " %\n";
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);

    std::cout << "Edge scratchpad: " << (kScratchpadBytes / 1024) << " KiB budget, peak use "
              << (scratchpad_peak_bytes / 1024.0) << " KiB"
              << (scratchpad_overflow ? "  [OVERFLOW: working set exceeds scratchpad]" : "")
              << "\n";

    std::cout << "Configuration:\n";
    std::cout << "Qubit Size: " << qubit_size << "\n";
    std::cout << "Grid Size: " << grid_row << "x" << grid_col << "\n";
    std::cout << "DRAM model: Ramulator 2.1, SOTA HBM4 (1 stack, 2.048 TB/s, 64 GB)\n";
    std::cout << "HBM Channels: " << kNumHBMChannels << " (accelerator-side striping)\n";

    std::cout << "Statistics saved to " << folder + output_name + ".power" << "\n";
    // Disable tracing (if enabled)
    try {
        PE::disableComputeTraceAggregate();
    } catch (...) {
        // ignore
    }
    try {
        hbmMemory.disableTraceAggregate();
    } catch (...) {
        // ignore
    }

    return 0;
}
