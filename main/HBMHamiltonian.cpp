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
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using GroupData = std::map<int, std::vector<std::tuple<double, int, int>>>;

namespace {
constexpr uint32_t kNumHBMChannels = 8;
constexpr uint64_t kBurstBytes = 64;
constexpr uint64_t kRowBytes = 2048;
constexpr uint64_t kChannelRowSpan = kRowBytes * kNumHBMChannels;
// SOTA HBM4 stack peak bandwidth (config/hbm4_sota.yaml: 2.048 TB/s = 2048 B/ns).
// Used only for the bulk scratchpad-spill cost estimate (see streamBytes).
constexpr double kHBM4PeakBytesPerNs = 2048.0;

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

// nnz-aware grouping: partition diagonals into ceil(D/maxPerGroup) groups, each
// with <= maxPerGroup diagonals (the array column/row bound), balancing total
// nonzeros per group (longest-processing-time first) so tiles carry similar PE
// work. Grouping only changes scheduling; the PE dataflow now produces the same
// result regardless of grouping (verified via -verify).
std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>
splitDiagonalsBalanced(
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals,
    int maxPerGroup) {
    using Vec = std::vector<std::tuple<double, int, int>>;
    std::vector<std::pair<int, Vec>> ds(diagonals.begin(), diagonals.end());
    std::sort(ds.begin(), ds.end(),
              [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });

    const int D = static_cast<int>(ds.size());
    if (D == 0 || maxPerGroup <= 0) return {};
    const int G = (D + maxPerGroup - 1) / maxPerGroup;

    std::vector<std::unordered_map<int, Vec>> groups(G);
    std::vector<size_t> load(G, 0), cnt(G, 0);
    for (auto& [off, vec] : ds) {
        int best = -1;
        size_t bestLoad = SIZE_MAX;
        for (int g = 0; g < G; ++g) {
            if (cnt[g] < static_cast<size_t>(maxPerGroup) && load[g] < bestLoad) {
                bestLoad = load[g];
                best = g;
            }
        }
        const size_t nnz = vec.size();
        groups[best][off] = std::move(vec);
        load[best] += nnz;
        ++cnt[best];
    }

    std::map<int, std::unordered_map<int, Vec>> out;
    for (int g = 0; g < G; ++g) {
        if (!groups[g].empty()) out[static_cast<int>(out.size())] = std::move(groups[g]);
    }
    return out;
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

    // Model a bulk scratchpad spill/refill of `sizeBytes` (data stays in SRAM for
    // correctness; only the HBM cost is charged). Per-burst cycle-accurate replay
    // through Ramulator is prohibitively slow for GB-scale spill, and a bulk
    // sequential spill is bandwidth-bound, so charge the bytes and a peak-BW time
    // estimate. (The per-tile operand/write-back traffic still uses Ramulator.)
    void streamBytes(size_t sizeBytes, bool isWrite) {
        if (isWrite) bytesWritten += sizeBytes; else bytesRead += sizeBytes;
        spillNs += static_cast<uint64_t>(static_cast<double>(sizeBytes) / kHBM4PeakBytesPerNs);
    }

    void erase(int groupIndex) {
        allocations.erase(groupIndex);
        storage.erase(groupIndex);
    }

    uint64_t getTotalCycles() const {
        return totalCycles + spillNs;
    }

    double tCK() const { return controller.tCK(); }   // ns per DRAM cycle

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
        return Stats{bytesRead, bytesWritten, totalCycles + spillNs};
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
    uint64_t spillNs = 0;   // accumulated analytic time for bulk scratchpad spills
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

// Byte-budgeted, write-back on-chip C scratchpad. The accumulated C data always
// lives on-chip (in `data_`, for correctness), but only groups whose total size
// fits within the budget are "resident"; touching a non-resident group refills
// it from HBM (spill read) and evicting a dirty group writes it back (spill
// write). This bounds on-chip C to the scratchpad budget and charges the real
// HBM traffic when the working set overflows. Set the budget each row via
// setBudget() (scratchpad minus the A double-buffer + resident B reservation).
class CScratchpad {
public:
    CScratchpad(HBMMemory& hbm, size_t totalBytes) : hbm_(hbm), total_(totalBytes) {}

    // Reserve part of the scratchpad for A/B buffers; C uses the remainder.
    void setReserved(size_t bytes) {
        reserved_ = bytes;
        budget_ = reserved_ < total_ ? total_ - reserved_ : 0;
        evictToBudget();
    }

    // Access a C group for read-modify-accumulate. Handles residency/spill and
    // returns a mutable reference to the authoritative on-chip data.
    GroupData& access(int gidx) {
        auto it = resident_.find(gidx);
        if (it != resident_.end()) {
            lru_.erase(it->second.pos);
            lru_.push_front(gidx);
            it->second.pos = lru_.begin();
            return data_[gidx];
        }
        GroupData& gd = data_[gidx];              // creates zero-size if brand new
        if (everEvicted_.count(gidx)) {           // real reload from HBM
            hbm_.streamBytes(computeGroupBytes(gd), /*isWrite=*/false);
            spillReads_++;
        }
        lru_.push_front(gidx);
        Entry e;
        e.pos = lru_.begin();
        e.bytes = computeGroupBytes(gd);
        curBytes_ += e.bytes;
        resident_[gidx] = e;
        evictToBudget();
        return data_[gidx];
    }

    // Refresh the accounted size of a group after it grew (accumulation may add
    // entries), then re-check the budget.
    void updateSize(int gidx) {
        auto it = resident_.find(gidx);
        if (it == resident_.end()) return;
        size_t nb = computeGroupBytes(data_[gidx]);
        curBytes_ += nb - it->second.bytes;
        it->second.bytes = nb;
        evictToBudget();
    }

    const std::map<int, GroupData>& groups() const { return data_; }
    size_t peakResidentBytes() const { return peakBytes_; }
    size_t peakTotalBytes() const { return peakTotal_; }   // reserved + resident C
    uint64_t spillReads() const { return spillReads_; }
    uint64_t spillWrites() const { return spillWrites_; }

private:
    struct Entry { std::list<int>::iterator pos; size_t bytes; };

    void evictToBudget() {
        peakBytes_ = std::max(peakBytes_, curBytes_);
        peakTotal_ = std::max(peakTotal_, reserved_ + curBytes_);
        // Keep at least one group resident to guarantee progress.
        while (curBytes_ > budget_ && lru_.size() > 1) {
            int victim = lru_.back();
            lru_.pop_back();
            auto it = resident_.find(victim);
            hbm_.streamBytes(it->second.bytes, /*isWrite=*/true);  // write-back
            spillWrites_++;
            everEvicted_.insert(victim);
            curBytes_ -= it->second.bytes;
            resident_.erase(it);
        }
    }

    HBMMemory& hbm_;
    size_t total_ = SIZE_MAX;      // full scratchpad size
    size_t reserved_ = 0;          // bytes reserved for A/B buffers
    size_t budget_ = SIZE_MAX;     // bytes available for C (= total_ - reserved_)
    std::map<int, GroupData> data_;                 // authoritative full C
    std::list<int> lru_;                            // MRU front
    std::unordered_map<int, Entry> resident_;
    std::unordered_set<int> everEvicted_;
    size_t curBytes_ = 0;
    size_t peakBytes_ = 0;
    size_t peakTotal_ = 0;
    uint64_t spillReads_ = 0, spillWrites_ = 0;
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
                    CScratchpad& C_scratch,
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
        const int gidx = C_offset_to_group.at(offset) + C_base;
        GroupData& groupData = C_scratch.access(gidx);   // residency/spill handled here
        auto& vec = groupData[offset];
        for (const auto& [value, i, j] : entries) {
            bool found = false;
            for (auto& [val, row, col] : vec) {
                if (row == i && col == j) { val += value; found = true; break; }
            }
            if (!found) vec.emplace_back(value, i, j);
        }
        C_scratch.updateSize(gidx);   // group may have grown
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

    // Tiling/ablation toggles (defaults = best measured config).
    const bool reuse   = args.count("reuse")   ? std::stoi(args["reuse"])   != 0 : true;   // #1 operand reuse
    const bool ctile   = args.count("ctile")   ? std::stoi(args["ctile"])   != 0 : true;   // #2 C budget/spill
    const bool balance = args.count("balance") ? std::stoi(args["balance"]) != 0 : false;  // #3 nnz-balanced grouping
    auto splitG = [balance](const std::unordered_map<int, std::vector<std::tuple<double,int,int>>>& d, int gc) {
        return balance ? splitDiagonalsBalanced(d, gc) : splitDiagonals(d, gc);
    };
    const std::string csv_path = args.count("csv") ? args["csv"] : "";
    bool verify_all_pass = true;   // AND of every -verify iteration (true if -verify off)

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
    if (args.count("optrace")) {
        PE::enableComputeTrace(args["optrace"]);   // debug: per-multiply (i,j,result) trace
    } else {
        PE::enableComputeTraceAggregate(compute_trace_path + ".aggregate.csv");
    }

    std::ofstream out;
    if (args.count("log")) {            // debug: dump full per-cycle PE trace
        out.open(args["log"]);
    } else {
        out.setstate(std::ios_base::failbit);
    }
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

    std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>> A_diag_groups = splitG(current_diag, grid_col);
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
    uint64_t c_spill_reads = 0, c_spill_writes = 0;

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
        C_diag_groups = splitG(C_diag, grid_col);
        std::unordered_map<int, int> C_offset_to_group = createOffsetToGroupMap(C_diag_groups);
        std::cout << "A offsets: ";
        for (const auto& offset : A_offsets) {
            std::cout << offset << " ";
        }
        std::cout << "\n";

        int A_base = 1000 * k;

        C_base = A_base + 1000;

        // C is accumulated in the on-chip scratchpad. With -ctile it is bounded to
        // the budget (groups that don't fit spill to HBM); without it, C is
        // unbounded on-chip (ablation: no capacity limit, no spill).
        CScratchpad C_scratch(hbmMemory, ctile ? kScratchpadBytes : SIZE_MAX);

        size_t maxA_bytes = 0, maxB_bytes = 0;  // largest A/B tile (sizes the buffers)

        double prev_compute_ns = 0.0;
        bool first_tile = true;
        int cycles = 0;

        // #1 operand reuse: B is invariant across the outer loop, so load all B
        // groups once and keep them resident; A_i is loaded once per row and
        // reused across all j. Removes the O(size_A * B_groups) redundant operand
        // traffic. Without -reuse, A_i and B_j are reloaded from HBM every tile.
        std::map<int, GroupData> B_resident;
        size_t B_resident_bytes = 0;
        if (reuse) {
            for (int j = 0; j < static_cast<int>(B_diag_groups.size()); ++j) {
                B_resident[j] = scheduler.requestGroup(B_base + j);   // one-time HBM load
                size_t bb = computeGroupBytes(B_resident[j]);
                B_resident_bytes += bb;
                maxB_bytes = std::max(maxB_bytes, bb);
            }
            for (int i = 0; i < size_A; ++i) {
                uint64_t dramBefore = hbmMemory.getStats().cycles;
                auto A_diag_local = scheduler.requestGroup(A_base + i);  // once per row
                double A_load_ns = static_cast<double>(hbmMemory.getStats().cycles - dramBefore);
                if (!first_tile) total_hidden_ns += std::min(prev_compute_ns, A_load_ns);
                first_tile = false;
                maxA_bytes = std::max(maxA_bytes, computeGroupBytes(A_diag_local));
                auto A_offsets_local = rebuildOffsets(A_diag_local);
                C_scratch.setReserved(2 * maxA_bytes + B_resident_bytes);

                double row_compute_ns = 0.0;
                for (int j = 0; j < static_cast<int>(B_diag_groups.size()); ++j) {
                    auto A_diag_copy = A_diag_local;             // run_test_case takes non-const refs
                    auto B_diag_copy = B_resident[j];
                    auto B_offsets_local = rebuildOffsets(B_diag_copy);
                    cycles = run_test_case(A_offsets_local, B_offsets_local, A_diag_copy, B_diag_copy,
                                           scheduler, C_base, C_offset_to_group, C_scratch, out, Energyout);
                    total_cycles += cycles;
                    row_compute_ns += static_cast<double>(cycles) / kAccelClockGHz;
                }
                total_compute_ns += row_compute_ns;
                prev_compute_ns = row_compute_ns;
            }
        } else {
            for (int i = 0; i < size_A; ++i) {
                for (int j = 0; j < static_cast<int>(B_diag_groups.size()); ++j) {
                    uint64_t dramBefore = hbmMemory.getStats().cycles;
                    auto A_diag_local = scheduler.requestGroup(A_base + i);  // reloaded every tile
                    auto B_diag_local = scheduler.requestGroup(B_base + j);
                    double load_ns = static_cast<double>(hbmMemory.getStats().cycles - dramBefore);
                    if (!first_tile) total_hidden_ns += std::min(prev_compute_ns, load_ns);
                    first_tile = false;
                    maxA_bytes = std::max(maxA_bytes, computeGroupBytes(A_diag_local));
                    maxB_bytes = std::max(maxB_bytes, computeGroupBytes(B_diag_local));
                    C_scratch.setReserved(2 * maxA_bytes + 2 * maxB_bytes);
                    auto A_offsets_local = rebuildOffsets(A_diag_local);
                    auto B_offsets_local = rebuildOffsets(B_diag_local);
                    cycles = run_test_case(A_offsets_local, B_offsets_local, A_diag_local, B_diag_local,
                                           scheduler, C_base, C_offset_to_group, C_scratch, out, Energyout);
                    total_cycles += cycles;
                    double t = static_cast<double>(cycles) / kAccelClockGHz;
                    total_compute_ns += t;
                    prev_compute_ns = t;
                }
            }
        }

        // Scratchpad occupancy: A double buffer (2x) + resident B + the
        // budget-bounded resident C region. C beyond the budget is spilled to
        // HBM (counted in the HBM stats), so peak on-chip use stays <= budget.
        (void)maxB_bytes;
        scratchpad_peak_bytes = std::max(scratchpad_peak_bytes, C_scratch.peakTotalBytes());
        if (C_scratch.spillWrites() > 0) scratchpad_overflow = true;  // C didn't fully fit
        c_spill_reads += C_scratch.spillReads();
        c_spill_writes += C_scratch.spillWrites();

        for (int i =0; i < old_size_A; ++i) {
            hbmMemory.erase(old_A_base + i);
        }
        if (k == 1) {
            old_A_base = A_base;
            old_size_A = size_A;
        }
        C_offsets.clear();
        results.clear();
        // Read the finalised C from the scratchpad (all accumulated groups).
        for (const auto& [gidx, groupData] : C_scratch.groups()) {
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
            compareDense(ref, C_scratch.groups(), size, 1e-6, maxAbsDiff, mismatches);
            if (mismatches != 0) verify_all_pass = false;
            std::cout << "[verify] iter " << k << ": "
                      << (mismatches == 0 ? "PASS" : "FAIL")
                      << " (max_abs_diff=" << maxAbsDiff
                      << ", mismatches=" << mismatches << ")\n";
        }

        C_diag_groups = splitG(results, grid_col);
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
    uint64_t mem_cycles = totalStats.cycles;                 // HBM time in ns
    const double hbm_tCK = hbmMemory.tCK();                  // ns per DRAM cycle
    const uint64_t hbm_dram_cycles =
        hbm_tCK > 0.0 ? static_cast<uint64_t>(mem_cycles / hbm_tCK) : 0;
    std::cout << "HBM Time: " << mem_cycles << " ns  (" << hbm_dram_cycles
              << " DRAM cycles @ tCK=" << hbm_tCK << " ns)\n";
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
              << (scratchpad_peak_bytes / 1024.0) << " KiB";
    if (scratchpad_overflow) {
        std::cout << "  [C spilled to HBM: " << c_spill_writes << " write-backs, "
                  << c_spill_reads << " refills"
                  << (scratchpad_peak_bytes > kScratchpadBytes
                          ? "; A/B buffers + one C group still exceed budget — use larger SPM or smaller grid_col"
                          : "")
                  << "]";
    } else {
        std::cout << "  [fit on-chip]";
    }
    std::cout << "\n";

    std::cout << "Configuration:\n";
    std::cout << "Qubit Size: " << qubit_size << "\n";
    std::cout << "Grid Size: " << grid_row << "x" << grid_col << "\n";
    std::cout << "DRAM model: Ramulator 2.1, SOTA HBM4 (1 stack, 2.048 TB/s, 64 GB)\n";
    std::cout << "HBM Channels: " << kNumHBMChannels << " (accelerator-side striping)\n";

    std::cout << "Statistics saved to " << folder + output_name + ".power" << "\n";

    // Machine-readable summary row (append to -csv=<path>; header written by caller).
    if (!csv_path.empty()) {
        std::ofstream csv(csv_path, std::ios::app);
        if (csv) {
            csv << output_name << ',' << qubit_size << ',' << grid_row << ',' << grid_col << ','
                << iterations << ',' << (reuse ? 1 : 0) << ',' << (ctile ? 1 : 0) << ','
                << (balance ? 1 : 0) << ','
                << total_cycles << ','
                << mem_cycles << ','          // HBM time (ns)
                << hbm_dram_cycles << ','     // HBM time (DRAM cycles)
                << totalStats.bytesRead << ',' << totalStats.bytesWritten << ','
                << std::fixed << std::setprecision(3) << mem_latency_pct << ','
                << std::setprecision(0) << (scratchpad_peak_bytes / 1024.0) << ','
                << c_spill_reads << ',' << c_spill_writes << ','
                << (verify ? (verify_all_pass ? "PASS" : "FAIL") : "n/a") << '\n';
        } else {
            std::cerr << "Warning: could not open csv path " << csv_path << "\n";
        }
    }

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
