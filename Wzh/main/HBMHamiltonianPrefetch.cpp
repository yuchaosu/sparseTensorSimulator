#include "../include/Grid.h"
#include "../include/Connection.h"
#include "../include/TreeReducer.h"
#include "../include/Utility.h"
#include "../include/DiagonalReduction.h"
#include "../include/HBM.h"
#include "../include/PE.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>

using GroupData = std::map<int, std::vector<std::tuple<double, int, int>>>;

namespace prefetch_sim {
namespace {
constexpr uint32_t kNumHBMChannels = 8;
constexpr uint64_t kBurstBytes = 64;
constexpr uint64_t kRowBytes = 2048;
constexpr double kAcceleratorFrequencyHz = 700.0e6;
constexpr double kClockPeriodNs = 1e9 / kAcceleratorFrequencyHz;

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
    size_t bytes = sizeof(uint32_t);
    for (const auto& [offset, entries] : data) {
        (void)offset;
        bytes += 1;
        bytes += 1;
        bytes += entries.size() * (sizeof(float) + 2 * sizeof(int));
    }
    return std::max<size_t>(bytes, sizeof(uint32_t));
}

size_t bytesToBursts(size_t bytes) {
    return static_cast<size_t>((bytes + kBurstBytes - 1) / kBurstBytes);
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

    double getTotalTimeNs() const {
        return static_cast<double>(totalCycles);
    }

    void printStats(std::ostream& os) const {
        os << "HBM total simulated time: " << totalCycles << " ns\n";
    }

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

    HBMController controller;
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

    double getTotalTimeNs() const {
        return memory.getTotalTimeNs();
    }

private:
    HBMMemory& memory;
};

class GroupPrefetchBuffer {
public:
    GroupPrefetchBuffer(HBMScheduler& schedulerRef,
                        std::string label,
                        int baseIndex,
                        int groupCount,
                        size_t capacityEntries)
        : scheduler(schedulerRef), name(std::move(label)), base(baseIndex),
          count(groupCount), capacity(std::max<size_t>(1, capacityEntries)),
          nextPrefetch(0) {}

    void warm() {
        if (count <= 0) {
            return;
        }
        prefetchAhead(static_cast<int>(capacity) - 1);
    }

    // Reuse-distance statistics (stack distance approximated by LRU position)
    double getAvgReuseDistance() const {
        return reuseCount == 0 ? 0.0 : static_cast<double>(totalReuseDistance) / static_cast<double>(reuseCount);
    }

    uint64_t getReuseCount() const { return reuseCount; }

    void prefetchAhead(int uptoIndex) {
        if (count <= 0) {
            return;
        }
        int target = std::min(std::max(uptoIndex, 0), count - 1);
        while (nextPrefetch <= target && nextPrefetch < count) {
            prefetch(nextPrefetch);
            ++nextPrefetch;
        }
    }

    GroupData getCopy(int index) {
        bool wasHit = ensure(index);
        auto it = cache.find(index);
        if (it == cache.end()) {
            throw std::runtime_error("Prefetch buffer access to missing entry");
        }
        touch(it, wasHit);
        return it->second.data;
    }

    // ensure returns true if the index was already in cache (a hit), false if it had to be prefetched
    bool ensure(int index) {
        if (index < 0 || index >= count) {
            throw std::out_of_range("Prefetch buffer index");
        }
        auto it = cache.find(index);
        if (it != cache.end()) {
            return true;
        }
        prefetch(index);
        return false;
    }

    void release(int index) {
        auto it = cache.find(index);
        if (it == cache.end()) {
            return;
        }
        lru.erase(it->second.orderIt);
        cache.erase(it);
    }

    size_t cached() const {
        return cache.size();
    }

private:
    struct Entry {
        GroupData data;
        std::list<int>::iterator orderIt;
    };

    void prefetch(int index) {
        if (index < 0 || index >= count) {
            return;
        }
        if (cache.find(index) != cache.end()) {
            return;
        }
        if (cache.size() >= capacity) {
            evictLRU();
        }
        GroupData payload = scheduler.requestGroup(base + index);
        lru.push_back(index);
        auto it = std::prev(lru.end());
        cache.emplace(index, Entry{std::move(payload), it});
    }

    void evictLRU() {
        if (lru.empty()) {
            return;
        }
        int victim = lru.front();
        lru.pop_front();
        cache.erase(victim);
    }

    void touch(std::unordered_map<int, Entry>::iterator it, bool isHit) {
        // compute reuse distance as number of entries more-recent than this one
        // Only record reuse-distance for true cache hits (not for the initial load/prefetch)
        if (isHit) {
            auto listIt = it->second.orderIt;
            uint64_t distance = 0;
            for (auto itr = std::next(listIt); itr != lru.end(); ++itr) {
                ++distance;
            }
            totalReuseDistance += distance;
            ++reuseCount;
        }

        lru.erase(it->second.orderIt);
        lru.push_back(it->first);
        it->second.orderIt = std::prev(lru.end());
    }

    HBMScheduler& scheduler;
    std::string name;
    int base;
    int count;
    size_t capacity;
    int nextPrefetch;
    std::unordered_map<int, Entry> cache;
    std::list<int> lru;
    // reuse-distance accumulators
    uint64_t totalReuseDistance = 0;
    uint64_t reuseCount = 0;
};

struct ExecutionMetrics {
    uint64_t cycles = 0;
    uint64_t flops = 0;
};

std::vector<std::vector<int>> generateReductionMap(
    const std::vector<int>& A_offsets,
    const std::vector<int>& B_offsets,
    std::vector<DiagonalReduction*>& diagonalReductions,
    std::ostream& out) {
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
                out << "Created DiagonalReduction for index: " << index << " from (A" << A_offsets[j]
                    << ", B" << B_offsets_reversed[i] << ")" << std::endl;
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
            (void)vec;
            offsetToGroup.emplace(offset, groupIdx);
        }
    }

    return offsetToGroup;
}

ExecutionMetrics run_test_case(
    const std::vector<int>& A_offsets,
    const std::vector<int>& B_offsets,
    GroupData A_diag,
    GroupData B_diag,
    HBMScheduler& scheduler,
    int C_base,
    const std::unordered_map<int, int>& C_offset_to_group,
    std::ofstream& out,
    std::ofstream& Energyout) {

    int COL = static_cast<int>(A_offsets.size());
    int ROW = static_cast<int>(B_offsets.size());

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

    Grid grid(ROW, COL, diagonalReductions, reductionMap, out);

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

    bool injection_done = false;
    while (true) {
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
        if (injection_done && grid.isIdle()) {
            out << "All data injected and processed. Breaking out of cycle loop.\n";
            break;
        }
        ++cycle;
        out << "Grid Idle: " << grid.isIdle() << "\n";
    }

    GroupData result = grid.getResults();
    for (const auto& [offset, entries] : result) {
        auto groupIt = C_offset_to_group.find(offset);
        if (groupIt == C_offset_to_group.end()) {
            continue;
        }
        int targetGroup = groupIt->second + C_base;
        GroupData groupData = scheduler.requestGroup(targetGroup);
        for (const auto& [value, i, j] : entries) {
            for (auto& [val, row, col] : groupData[offset]) {
                if (row == i && col == j) {
                    val += value;
                    break;
                }
            }
        }
        scheduler.storeGroup(targetGroup, groupData);
    }
    grid.printEnergy(Energyout);
    out << "Total Cycles: " << cycle << "\n";

    uint64_t flopCount = 0;
    for (int i = 0; i < ROW; ++i) {
        for (int j = 0; j < COL; ++j) {
            flopCount += grid.getPE(i, j)->getMultiplyCount();
        }
    }

    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    return ExecutionMetrics{static_cast<uint64_t>(cycle), flopCount};
}

} // namespace
} // namespace prefetch_sim

int main(int argc, char* argv[]) {
    using namespace prefetch_sim;

    uint64_t total_cycles = 0;
    uint64_t total_compute_cycles = 0;
    uint64_t total_flops = 0;
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
    size_t defaultBuffer = args.count("buffer") ? std::stoul(args["buffer"]) : 4;
    size_t bufferA = args.count("bufferA") ? std::stoul(args["bufferA"]) : defaultBuffer;
    size_t bufferB = args.count("bufferB") ? std::stoul(args["bufferB"]) : defaultBuffer;
    std::string rooflineCsv = args.count("roofcsv") ? args["roofcsv"] : "";
    bool verbose = false;
    bool debug = false;
    if (args.count("verbose")) {
        const std::string& v = args["verbose"];
        verbose = !(v == "0" || v == "false" || v == "False" || v == "FALSE");
    }
    if (args.count("debug")) {
        const std::string& d = args["debug"];
        debug = !(d == "0" || d == "false" || d == "False" || d == "FALSE");
        verbose = true;
    }

    HBMMemory hbmMemory;
    HBMScheduler scheduler(hbmMemory);

    std::string output_name = filename;
    size_t dot_pos = output_name.find_last_of(".");
    if (dot_pos != std::string::npos) {
        output_name = output_name.substr(0, dot_pos);
    }
    folder = "/mnt/beegfs/ysu34/hamlib/" + folder;
    if (verbose) {
        std::cout << "filename: " << folder + filename << "\n";
    }
    std::string basePath = folder;

    if (!rooflineCsv.empty() && rooflineCsv.rfind("/", 0) != 0) {
        rooflineCsv = folder + rooflineCsv;
    }

    std::ofstream out;
    out.setstate(std::ios_base::failbit);
    std::ofstream Energyout(folder + output_name + ".power");

    std::string filenameA = basePath + filename;
    std::vector<int> A_offsets = extractDiagonalOffsets(filenameA);
    auto current_diag = createDiagonalMap(filenameA, A_offsets, size);

    auto B_offsets = A_offsets;

    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = splitDiagonals(current_diag, grid_col);
    auto B_diag_groups = A_diag_groups;
    for (const auto& [groupIndex, groupDataRaw] : A_diag_groups) {
        GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
        hbmMemory.initialStore(groupIndex, groupData);
    }
    int size_A = static_cast<int>(A_diag_groups.size());
    int size_B = static_cast<int>(B_diag_groups.size());
    int B_base = 0;
    std::vector<int> C_offsets;
    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> C_diag_groups;
    int C_base = 0;
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> results;
    int old_size_A = 0;
    int old_A_base = 0;
    HBMMemory::Stats prevStats = hbmMemory.getStats();

    size_t effectiveBufferA = std::max<size_t>(1, bufferA);
    size_t effectiveBufferB = std::max<size_t>(1, bufferB);
    int totalbytes = 0;
    for (int k = 0; k < iterations; ++k) {
        std::cout << "[INFO] Iteration " << (k + 1) << "/" << iterations << " begin" << std::endl;

        uint64_t iteration_cycles = 0;
        uint64_t iteration_flops = 0;

        if (k == 0) {
            if (verbose) {
                std::cout << "A diagonal size:" << A_offsets.size() << "\n";
            }
            C_offsets = computeResultDiagonals(A_offsets, B_offsets, size);
        } else {
            if (verbose) {
                std::cout << "A diagonal size:" << C_offsets.size() << "\n";
            }
            C_offsets = computeResultDiagonals(C_offsets, B_offsets, size);
        }
        auto C_diag = initializeCdiagGroups(C_offsets, size);
        C_diag_groups = splitDiagonals(C_diag, grid_col);
        std::unordered_map<int, int> C_offset_to_group = createOffsetToGroupMap(C_diag_groups);
        if (verbose) {
            std::cout << "A offsets: ";
            for (const auto& offset : A_offsets) {
                std::cout << offset << " ";
            }
            std::cout << "\n";
        }

        int A_base = 1000 * k;
        C_base = A_base + 1000;

        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            hbmMemory.initialStore(C_base + groupIndex, groupData);
        }

        GroupPrefetchBuffer aBuffer(scheduler, "A", A_base, size_A, effectiveBufferA);
        GroupPrefetchBuffer bBuffer(scheduler, "B", B_base, size_B, effectiveBufferB);
        aBuffer.warm();
        bBuffer.warm();

        const int blockSizeA = static_cast<int>(std::min<size_t>(effectiveBufferA, static_cast<size_t>(std::max(1, size_A))));
        const int blockSizeB = static_cast<int>(std::min<size_t>(effectiveBufferB, static_cast<size_t>(std::max(1, size_B))));

        for (int bBase = 0; bBase < size_B; bBase += blockSizeB) {
            int bEnd = std::min(size_B, bBase + blockSizeB);
            if (bEnd <= bBase) {
                break;
            }
            bBuffer.prefetchAhead(bEnd - 1);
            if (debug) {
                std::cout << "[DEBUG] Prefetched B block [" << bBase << ", " << bEnd << ")" << std::endl;
            }

            std::vector<GroupData> bBlockData;
            std::vector<std::vector<int>> bBlockOffsets;
            bBlockData.reserve(static_cast<size_t>(bEnd - bBase));
            bBlockOffsets.reserve(static_cast<size_t>(bEnd - bBase));
            for (int bj = bBase; bj < bEnd; ++bj) {
                GroupData data = bBuffer.getCopy(bj);
                bBlockOffsets.push_back(rebuildOffsets(data));
                bBlockData.push_back(data);
            }

            for (int aBase = 0; aBase < size_A; aBase += blockSizeA) {
                int aEnd = std::min(size_A, aBase + blockSizeA);
                if (aEnd <= aBase) {
                    break;
                }
                aBuffer.prefetchAhead(aEnd - 1);
                if (debug) {
                    std::cout << "[DEBUG] Prefetched A block [" << aBase << ", " << aEnd << ")" << std::endl;
                }

                std::vector<GroupData> aBlockData;
                std::vector<std::vector<int>> aBlockOffsets;
                aBlockData.reserve(static_cast<size_t>(aEnd - aBase));
                aBlockOffsets.reserve(static_cast<size_t>(aEnd - aBase));
                for (int ai = aBase; ai < aEnd; ++ai) {
                    GroupData data = aBuffer.getCopy(ai);
                    aBlockOffsets.push_back(rebuildOffsets(data));
                    aBlockData.push_back(data);
                }

                for (size_t aiIdx = 0; aiIdx < aBlockData.size(); ++aiIdx) {
                    for (size_t bjIdx = 0; bjIdx < bBlockData.size(); ++bjIdx) {
                        if (debug) {
                            std::cout << "[DEBUG] Launch multiply A#" << (aBase + static_cast<int>(aiIdx))
                                      << " x B#" << (bBase + static_cast<int>(bjIdx)) << std::endl;
                        }
                        ExecutionMetrics metrics = run_test_case(
                            aBlockOffsets[aiIdx],
                            bBlockOffsets[bjIdx],
                            aBlockData[aiIdx],
                            bBlockData[bjIdx],
                            scheduler,
                            C_base,
                            C_offset_to_group,
                            out,
                            Energyout
                        );
                        total_cycles += metrics.cycles;
                        total_compute_cycles += metrics.cycles;
                        total_flops += metrics.flops;
                        iteration_cycles += metrics.cycles;
                        iteration_flops += metrics.flops;
                        if (debug) {
                            std::cout << "[DEBUG]  -> cycles " << metrics.cycles
                                      << ", flops " << metrics.flops << std::endl;
                        }
                    }
                }

                for (int ai = aBase; ai < aEnd; ++ai) {
                    aBuffer.release(ai);
                }
                if (aEnd < size_A) {
                    int nextTarget = std::min(size_A - 1, aEnd + blockSizeA - 1);
                    aBuffer.prefetchAhead(nextTarget);
                }
            }

            for (int bj = bBase; bj < bEnd; ++bj) {
                bBuffer.release(bj);
            }
            if (bEnd < size_B) {
                int nextTarget = std::min(size_B - 1, bEnd + blockSizeB - 1);
                bBuffer.prefetchAhead(nextTarget);
                if (debug) {
                    std::cout << "[DEBUG] Advanced B window to cover index " << nextTarget << std::endl;
                }
            }
        }

            // Print prefetch buffer reuse-distance statistics so we can see cache effects
            std::cout << std::fixed << std::setprecision(3);
            std::cout << "Prefetch A buffer avg reuse distance: " << aBuffer.getAvgReuseDistance()
                      << " (count=" << aBuffer.getReuseCount() << ")\n";
            std::cout << "Prefetch B buffer avg reuse distance: " << bBuffer.getAvgReuseDistance()
                      << " (count=" << bBuffer.getReuseCount() << ")\n";

            for (int i = 0; i < old_size_A; ++i) {
            hbmMemory.erase(old_A_base + i);
        }
        if (k == 1) {
            old_A_base = A_base;
            old_size_A = size_A;
        }
        C_offsets.clear();
        results.clear();
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData = hbmMemory.load(C_base + groupIndex);
            hbmMemory.erase(C_base + groupIndex);
            for (const auto& [offset, entries] : groupData) {
                bool allZero = true;
                for (const auto& [value, i, j] : entries) {
                    if (value != 0.0) {
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
        C_diag_groups = splitDiagonals(results, grid_col);
        for (const auto& [groupIndex, groupDataRaw] : C_diag_groups) {
            GroupData groupData(groupDataRaw.begin(), groupDataRaw.end());
            scheduler.storeGroup(C_base + groupIndex, groupData);
        }
        size_A = static_cast<int>(C_diag_groups.size());

        
            std::cout << "Matrix diagonal size: " << C_offsets.size() << "\n";
            std::cout << "Diagonal";
            for (const auto& offset : C_offsets) {
                std::cout << offset << " ";
                if (offset >= 0)
                    totalbytes += sizeof(double) * (size - offset);
                else
                    totalbytes += sizeof(double) * (size + offset);
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

        if (verbose) {
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "HBM iteration stats: Read " << (deltaRead / (1024.0 * 1024))
                      << " MiB, Write " << (deltaWrite / (1024.0 * 1024))
                      << " MiB, Bandwidth " << iterationBandwidth << " GiB/s\n";
            std::cout.unsetf(std::ios::floatfield);
            std::cout << std::setprecision(6);
            std::cout << "Cycles (iteration): " << iteration_cycles << ", Flops: " << iteration_flops << "\n";
            std::cout << "Finished multiplication with matrix_output_" << k << ".txt\n";
            std::cout << "------------------------------------------"<< std::endl;
        }
        std::cout << "[INFO] Iteration " << (k + 1) << " complete" << std::endl;

        prevStats = currentStats;
        A_offsets = C_offsets;
    }

    out << "Total cycles: " << total_cycles << "\n";
    HBMMemory::Stats totalStats = hbmMemory.getStats();
    out << "HBM total time: " << totalStats.cycles << " ns\n";
    out << "HBM bytes read: " << totalStats.bytesRead << ", bytes written: "
        << totalStats.bytesWritten << "\n";

    double totalBytes = static_cast<double>(totalStats.bytesRead + totalStats.bytesWritten);
    double mem_time_sec = static_cast<double>(totalStats.cycles) / 1e9;
    double accel_time_sec = static_cast<double>(total_compute_cycles) * kClockPeriodNs / 1e9;
    double exposed_mem_latency_sec = std::max(mem_time_sec - accel_time_sec, 0.0);
    double runtime_sec = accel_time_sec + exposed_mem_latency_sec;
    double mem_bandwidth_gbps = (runtime_sec > 0.0) ? (totalBytes / runtime_sec) / 1e9 : 0.0;

    if (!rooflineCsv.empty()) {
        std::ofstream roofCsv(rooflineCsv);
        if (roofCsv.is_open()) {
            roofCsv << "label,flops,bytes,time_sec\n";
            roofCsv << output_name << ","
                    << static_cast<double>(total_flops) << ","
                    << totalBytes << ","
                    << std::setprecision(9) << runtime_sec << "\n";
            roofCsv.close();
        } else if (verbose) {
            std::cerr << "Unable to write roofline CSV at " << rooflineCsv << "\n";
        }
    }

    if (verbose) {
        std::cout << "Total cycles: " << total_cycles << "\n";
        std::cout << "HBM time (s): " << mem_time_sec << ", accelerator time (s): " << accel_time_sec << "\n";
        std::cout << "Prefetch buffer A: " << effectiveBufferA << " entries, B: " << effectiveBufferB << " entries\n";
        std::cout << "Statistics saved to " << folder + output_name + ".power" << "\n";
    }
    double flops_per_byte = (totalbytes > 0.0) ? (static_cast<double>(total_flops) / totalbytes) : 0.0;
    double gflops = 0.0;
    if (accel_time_sec > 0.0) {
        gflops = (static_cast<double>(total_flops) / accel_time_sec) / 1e9;
    }
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Accelerator_Time_s: " << accel_time_sec << "\n";
    std::cout << "HBM_Exposed_Latency_s: " << exposed_mem_latency_sec << "\n";
    std::cout << "HBM_Time_s: " << mem_time_sec << "\n";
    std::cout << "Total_Runtime_s: " << runtime_sec << "\n";
    std::cout << "Total_FLOPS: " << static_cast<double>(total_flops) << "\n";
    std::cout << "Total_Bytes_Moved: " << static_cast<unsigned long long>(totalbytes) << "\n";
    std::cout << "HBM_Bandwidth_GBps: " << mem_bandwidth_gbps << "\n";
    std::cout << "GFLOPS: " << gflops << "\n";
    std::cout << "flops_per_byte: " << flops_per_byte << "\n";

    // Record summary CSV: matrix name, FLOPS/byte, GFLOPS, bytes moved, runtime
    

    // Write a single human-readable summary file (overwrite each run)
    std::string localSummary = std::string("./") + "summary.txt";
    // Append to the summary file instead of overwriting so multiple runs accumulate
    std::ofstream summary(localSummary, std::ios::app);
    if (summary.is_open()) {
        summary << "matrix: " << filename << "\n";
        summary << std::fixed << std::setprecision(9);
        summary << "flops_per_byte: " << flops_per_byte << "\n";
        summary << "gflops: " << gflops << "\n";
        summary << "bytes_moved: " << static_cast<unsigned long long>(totalBytes) << "\n";
        summary << "accelerator_time_s: " << accel_time_sec << "\n";
        summary << "hbm_time_s: " << mem_time_sec << "\n";
        summary << "total_runtime_s: " << runtime_sec << "\n";
        summary << "total_flops: " << static_cast<double>(total_flops) << "\n";
        summary << "GFLOPS: " << gflops << "\n";
        summary << "flops_per_byte: " << flops_per_byte << "\n";
        summary.close();
        if (verbose) std::cout << "Summary written to " << localSummary << "\n";
    } else if (verbose) {
        std::cerr << "Failed to write summary to " << localSummary << "\n";
    }

    return 0;
}
