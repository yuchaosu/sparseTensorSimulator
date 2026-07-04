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
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <memory>
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

// One tile simulation result: cycles to drain the grid, and how many of those
// cycles the injector was stalled by a full edge FIFO (finite-buffer backpressure).
struct RunResult { int cycles; long stall_cycles; };

// fifo_depth models the finite per-PE input buffer. commit=true accumulates the
// tile's partial products into the C scratchpad (and thus touches HBM via spill);
// commit=false is a pure timing pass (used to measure the idealized unbounded-FIFO
// cycle count without double-counting memory traffic or results).
RunResult run_test_case( const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& A_diag,
                    std::map<int, std::vector<std::tuple<double, int, int>>>& B_diag,
                    HBMScheduler& scheduler, int C_base, std::unordered_map<int, int> C_offset_to_group,
                    CScratchpad& C_scratch,
                    std::ofstream& out, std::ofstream& Energyout,
                    size_t fifo_depth, bool commit) {

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

    Grid grid(ROW, COL, diagonalReductions,reductionMap, out, fifo_depth);

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
    long stall_cycles = 0;
    while(true) {
        out << "===== Cycle " << cycle << " =====\n";

        // A tick "stalls" when some eligible operand is ready to inject but the edge
        // connection is still pending — which (injection runs before grid.cycle, so
        // the connection reflects last tick's state) means the edge PE could not
        // drain it because its input FIFO was full. That is finite-buffer backpressure.
        bool eligible_blocked = false;

        for (int col = 0; col < COL; ++col) {
            if (col < static_cast<int>(A_diag_packages.size()) && cycle >= col) {
                int& idx = A_inject_index[col];
                const auto& vec = A_diag_packages[col];

                if (idx < static_cast<int>(vec.size())) {
                    if (!top_in[col]->pendingSrc()) {
                        top_in[col]->receiveSrc(vec[idx]);
                        out << "Injecting A diagonal package at column " << col << ": " << vec[idx] << "\n";
                        ++idx;

                        if (idx == static_cast<int>(vec.size())) {
                            top_in[col]->receiveInjectionFinished(true);
                            out << "Injecting per-column finish signal at column " << col << ": true\n";
                        }
                    } else {
                        eligible_blocked = true;
                    }
                }
            }
        }

        for (int row = 0; row < ROW; ++row) {
            if (row < static_cast<int>(B_diag_packages.size()) && cycle >= row) {
                int& idx = B_inject_index[row];
                const auto& vec = B_diag_packages[row];

                if (idx < static_cast<int>(vec.size())) {
                    if (!left_in[row]->pendingSrc()) {
                        left_in[row]->receiveSrc(vec[idx]);
                        out << "Injecting B diagonal package at row " << row << ": " << vec[idx] << "\n";
                        ++idx;

                        if (idx == static_cast<int>(vec.size())) {
                            left_in[row]->receiveInjectionFinished(true);
                            out << "Injecting per-row finish signal at row " << row << ": true\n";
                        }
                    } else {
                        eligible_blocked = true;
                    }
                }
            }
        }

        if (eligible_blocked) ++stall_cycles;

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
    // per group at the end of the iteration.) Only on the committing pass — the
    // ideal/unbounded timing pass must not touch C or HBM.
    if (commit) {
        GroupData result = grid.getResults();
        for (const auto& [offset, entries] : result) {
            const int gidx = C_offset_to_group.at(offset) + C_base;
            GroupData& groupData = C_scratch.access(gidx);   // residency/spill handled here
            auto& vec = groupData[offset];
            // Accumulate this tile's (row,col)->value into vec. Use a hash index on
            // (row,col) instead of a linear scan: the old O(entries*vec) scan is
            // O(n^2) for a single large tile (q>=18 has n up to millions).
            std::unordered_map<long long, size_t> pos;
            pos.reserve(vec.size() * 2 + 1);
            auto keyOf = [](int row, int col) {
                return (static_cast<long long>(row) << 32) | static_cast<unsigned int>(col);
            };
            for (size_t idx = 0; idx < vec.size(); ++idx)
                pos[keyOf(std::get<1>(vec[idx]), std::get<2>(vec[idx]))] = idx;
            for (const auto& [value, i, j] : entries) {
                long long key = keyOf(i, j);
                auto it = pos.find(key);
                if (it != pos.end()) std::get<0>(vec[it->second]) += value;
                else { pos[key] = vec.size(); vec.emplace_back(value, i, j); }
            }
            C_scratch.updateSize(gidx);   // group may have grown
        }
        grid.printEnergy(Energyout);
    }
    out << "Total Cycles: " << cycle << "\n";

    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    return RunResult{cycle, stall_cycles};
}

// Generic connected-mesh tile: the SHARED real-hardware engine for every mesh dataflow.
// Builds a ROW x COL real PE Grid + reductions, injects A fibers at the top (columns) and
// B fibers at the left (rows), cycles to drain, and accumulates the reduction's output-
// keyed results into out_C. All routing/buffering/reduction go through real
// PE/Connection/DiagonalReduction objects, so the activity counters are MEASURED (not
// computed) -- identical methodology to the diagonal (ours) and dense (TPU) runs. The
// caller pre-sorts each fiber by the contraction index: A by index2, B by index1 (the
// merge-join matches A.index2==B.index1); the product carries (A.index1, B.index2).
static long runMeshTile(const std::vector<int>& A_labels, const std::vector<int>& B_labels,
                        std::vector<std::vector<DataPackage>> A_pk,
                        std::vector<std::vector<DataPackage>> B_pk,
                        std::map<std::pair<int,int>, double>& out_C) {
    static std::ofstream devnull;
    const int COL = static_cast<int>(A_pk.size()), ROW = static_cast<int>(B_pk.size());
    if (COL == 0 || ROW == 0) return 0;
    std::vector<DiagonalReduction*> reductions;
    auto reductionMap = generateReductionMap(A_labels, B_labels, reductions, devnull);
    Grid grid(ROW, COL, reductions, reductionMap, devnull, 20000);
    std::vector<Connection*> left_in(ROW), top_in(COL);
    for (int i = 0; i < ROW; ++i) left_in[i] = new Connection(devnull);
    for (int j = 0; j < COL; ++j) top_in[j] = new Connection(devnull);
    grid.setInputConnections(top_in, left_in);
    std::reverse(B_pk.begin(), B_pk.end());          // match generateReductionMap's B reversal
    int cycle = 0;
    std::vector<int> A_idx(COL, 0), B_idx(ROW, 0);
    while (true) {
        for (int col = 0; col < COL; ++col)
            if (cycle >= col && A_idx[col] < (int)A_pk[col].size() && !top_in[col]->pendingSrc()) {
                top_in[col]->receiveSrc(A_pk[col][A_idx[col]]);
                if (++A_idx[col] == (int)A_pk[col].size()) top_in[col]->receiveInjectionFinished(true);
            }
        for (int row = 0; row < ROW; ++row)
            if (cycle >= row && B_idx[row] < (int)B_pk[row].size() && !left_in[row]->pendingSrc()) {
                left_in[row]->receiveSrc(B_pk[row][B_idx[row]]);
                if (++B_idx[row] == (int)B_pk[row].size()) left_in[row]->receiveInjectionFinished(true);
            }
        bool injection_done = true;
        for (int i = 0; i < ROW; ++i) if (left_in[i]->pendingSrc()) injection_done = false;
        for (int j = 0; j < COL; ++j) if (top_in[j]->pendingSrc()) injection_done = false;
        grid.cycle(cycle);
        if (injection_done && grid.isIdle()) break;
        ++cycle;
    }
    auto res = grid.getResults();
    for (auto& [idx, entries] : res)
        for (auto& [v, i1, i2] : entries) out_C[{i1, i2}] += v;
    for (auto* c : left_in) delete c;
    for (auto* c : top_in) delete c;
    for (auto* r : reductions) delete r;
    return cycle;
}

// Load a HamLib DIA-format matrix (hamlib/dia_oom/*.txt), produced by
// fetchham_sparse_dia.py:
//   line 1 : "N <n> D <numdiag>"
//   per row: "<offset>: v0 v1 ... v_{n-|offset|-1}"   (dense values along the
//            diagonal; the k-th value sits at position p=min(row,col)=k)
// Fills `offsets` (sorted ascending) and `diag` (offset -> nonzero (val,row,col),
// already row-sorted). Zeros in the file are dropped. Returns n, or -1 if the file
// is not DIA / unreadable. This is diagonal-native, so no (row,col) reconstruction
// or dense matrix is needed -- it scales to the large (q=18..) oom matrices.
static long loadDiaMatrix(const std::string& filename,
                          std::vector<int>& offsets,
                          std::unordered_map<int, std::vector<std::tuple<double,int,int>>>& diag) {
    std::ifstream in(filename);
    if (!in) return -1;
    std::string first;
    if (!std::getline(in, first)) return -1;
    long n = 0, D = 0;
    {
        std::istringstream iss(first);
        std::string tok;
        if (!(iss >> tok) || tok != "N") return -1;
        if (!(iss >> n)) return -1;
        if (!(iss >> tok) || tok != "D") return -1;
        iss >> D;
    }
    if (n <= 0) return -1;
    offsets.clear();
    diag.clear();
    std::string line;
    while (std::getline(in, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        int off = std::stoi(line.substr(0, colon));
        const long len = n - std::labs(static_cast<long>(off));
        if (len <= 0) continue;
        std::vector<std::tuple<double,int,int>> entries;
        const char* p = line.c_str() + colon + 1;
        char* end = nullptr;
        for (long k = 0; k < len; ++k) {
            double v = std::strtod(p, &end);
            if (end == p) break;              // ran out of numbers on this line
            p = end;
            if (v != 0.0) {
                int row, col;
                if (off >= 0) { row = static_cast<int>(k);       col = static_cast<int>(k + off); }
                else          { row = static_cast<int>(k - off); col = static_cast<int>(k); }
                entries.emplace_back(v, row, col);   // pushed k-ascending => row-sorted
            }
        }
        if (!entries.empty()) {
            offsets.push_back(off);
            diag[off] = std::move(entries);
        }
    }
    std::sort(offsets.begin(), offsets.end());
    return n;
}

// Offset-space convolution C = A @ B for diagonal-stored operands. Returns C as a
// diagonal map (offset -> row-sorted nonzero (val,row,col)); via out-params gives the
// analytic PE makespan under the levers, total nonzero MACs, and the peak |offset|.
//   makespan: balance ? ceil(total_work / #pairs) : max over pairs of work,
//             where work = nonzero products (zeroskip) or full overlap length.
using ConvDiag = std::unordered_map<int, std::vector<std::tuple<double,int,int>>>;
static ConvDiag convMatmul(const ConvDiag& A, const ConvDiag& B, int n,
                           bool zeroskip, bool balance,
                           long& makespan_out, long long& nnz_out, int& maxoff_out,
                           long& work_out, int pe_budget = 0) {
    // pe_budget > 0 caps the PE count (for equal-PE comparison); with balancing the
    // makespan is ceil(total_work / PEs). pe_budget == 0 uses one PE per diagonal pair.
    // Sparse: B as offset -> (row -> val) for O(1) aligned lookup; iterate only the
    // NONZEROS of A. Memory is O(nnz), not O(#offsets * n) -- essential once H^k
    // fills in many diagonals (the dense-vector version OOMs).
    std::unordered_map<int, std::unordered_map<int, double>> Bmap;
    for (const auto& [dB, ents] : B) {
        auto& m = Bmap[dB]; m.reserve(ents.size() * 2 + 1);
        for (const auto& [val, r, c] : ents) m[r] = val;
    }
    std::unordered_map<int, std::unordered_map<int, double>> Cmap;   // dC -> (row -> val)
    long long total_len = 0, total_nnz = 0, maxpair_len = 0, maxpair_nnz = 0;
    long pairs = 0; int maxoff = 0;
    for (const auto& [dA, aents] : A) {
        for (const auto& [dB, bm] : Bmap) {
            const int dC = dA + dB;
            const int lo = std::max(0, std::max(-dA, -dC));
            const int hi = std::min(n, std::min(n - dA, n - dC));
            if (hi <= lo) continue;
            ++pairs;
            long len = hi - lo, nnz = 0;
            auto& cm = Cmap[dC];
            for (const auto& [aval, rA, cA] : aents) {   // rA = A row; A.col == B.row => B row = rA+dA
                if (rA < lo || rA >= hi) continue;
                auto it = bm.find(rA + dA);
                if (it != bm.end()) {
                    double p = aval * it->second;
                    if (p != 0.0) { cm[rA] += p; ++nnz; }
                }
            }
            total_len += len; total_nnz += nnz;
            maxpair_len = std::max<long long>(maxpair_len, len);
            maxpair_nnz = std::max<long long>(maxpair_nnz, nnz);
            maxoff = std::max(maxoff, std::abs(dC));
        }
    }
    const long long work = zeroskip ? total_nnz : total_len;
    work_out = static_cast<long>(work);
    if (balance) {
        long pe = pe_budget > 0 ? pe_budget : static_cast<long>(pairs);
        makespan_out = pe ? static_cast<long>((work + pe - 1) / pe) : 0;
    } else {
        makespan_out = static_cast<long>(zeroskip ? maxpair_nnz : maxpair_len);
    }
    nnz_out = total_nnz; maxoff_out = maxoff;
    ConvDiag C;
    for (auto& [dC, cm] : Cmap) {
        std::vector<std::tuple<double,int,int>> ents;
        ents.reserve(cm.size());
        for (auto& [row, val] : cm) if (val != 0.0) ents.emplace_back(val, row, row + dC);
        std::sort(ents.begin(), ents.end(), [](const auto& x, const auto& y){ return std::get<1>(x) < std::get<1>(y); });
        if (!ents.empty()) C[dC] = std::move(ents);
    }
    return C;
}

// Cycle-accurate convolution ON the real PE array. Each (dA,dB) pair is one PE fed
// its OFFSET-ALIGNED operand pair (A element row k paired with B element row k+dA),
// so the existing merge-join PE::cycle() finds A.col==B.row on every head and does a
// MAC + advance-both each cycle -- no index-search stalls. Makespan is the simulated
// number of lockstep cycles, not a formula. This genuinely uses the Grid/PE hardware;
// it is a small-q validation of the analytic conv model (same makespan expected).
static ConvDiag convOnGrid(const ConvDiag& A, const ConvDiag& B, int n,
                           bool zeroskip, long& cycles_out) {
    static std::ofstream devnull;   // unopened: PE's out<< become cheap no-ops
    auto toVec = [&](const ConvDiag& M) {
        std::unordered_map<int, std::vector<double>> v;
        for (const auto& [o, e] : M) { auto& a = v[o]; a.assign(n, 0.0); for (const auto& [val, r, c] : e) a[r] = val; }
        return v;
    };
    auto Av = toVec(A), Bv = toVec(B);
    std::vector<std::unique_ptr<PE>> pes;
    for (const auto& [dA, va] : Av) {
        for (const auto& [dB, vb] : Bv) {
            const int dC = dA + dB;
            const int lo = std::max(0, std::max(-dA, -dC));
            const int hi = std::min(n, std::min(n - dA, n - dC));
            if (hi <= lo) continue;
            auto pe = std::make_unique<PE>(0, 0, devnull, static_cast<size_t>(hi - lo) + 16);
            pe->setLastRow(true); pe->setLastCol(true);   // boundary: drain, don't forward
            for (int k = lo; k < hi; ++k) {
                double a = va[k], b = vb[k + dA];
                if (zeroskip && a * b == 0.0) continue;
                pe->receivedA.push(DataPackage(a, k, k + dA));            // A: row k, col k+dA
                pe->receivedB.push(DataPackage(b, k + dA, k + dA + dB));  // B: row k+dA, col k+dC
            }
            if (!pe->receivedA.isEmpty()) pes.push_back(std::move(pe));
        }
    }
    std::unordered_map<int, std::unordered_map<int, double>> acc;   // dC -> row -> value
    auto drain = [&](PE& pe) {
        while (!pe.PsumOut.isEmpty()) {
            DataPackage p = pe.PsumOut.front(); pe.PsumOut.pop();
            acc[p.index2 - p.index1][p.index1] += p.value;          // dC = col - row
        }
    };
    long cyc = 0; bool any = true;
    while (any) {
        any = false;
        for (auto& pe : pes) {
            if (!pe->receivedA.isEmpty()) { pe->cycle(static_cast<uint64_t>(cyc)); any = true; }
            drain(*pe);
        }
        if (any) ++cyc;
    }
    for (auto& pe : pes) drain(*pe);   // flush last products
    cycles_out = cyc;
    ConvDiag C;
    for (auto& [dC, m] : acc) {
        std::vector<std::tuple<double,int,int>> ents;
        for (auto& [row, val] : m) if (val != 0.0) ents.emplace_back(val, row, row + dC);
        std::sort(ents.begin(), ents.end(), [](auto& x, auto& y){ return std::get<1>(x) < std::get<1>(y); });
        if (!ents.empty()) C[dC] = std::move(ents);
    }
    return C;
}

// Cycle-accurate DENSE GEMM on the REAL PE mesh -- the same PE::cycle() the sparse
// dataflows use. Feeding PE(i,j) the k-aligned streams A[i][.] (as (val, i, k), col=k)
// and B[.][j] (as (val, k, j), row=k) makes the merge-join match on every k and MAC
// A[i][k]*B[k][j] -- a genuine output-stationary systolic cell. The n x n output is
// tiled into S x S blocks run one at a time on the physical array (cycles accumulate),
// so this measures the real hardware doing a full dense multiply (no sparsity skipped).
// Returns lockstep cycles for ONE dense n x n GEMM. Feasible only for small n (a dense
// GEMM is n^3/S^2 cycles) -- used to validate the analytic `tpu` model. Isolated
// boundary PEs, so the streaming term (n/tile) is exact; systolic fill/drain (~2S/tile,
// negligible for n>>S) is added by the analytic model, not simulated here (as in
// convgrid).
static long long tpuOnGrid(const DenseMat& A, const DenseMat& B, int n, int S) {
    static std::ofstream devnull;
    long long total = 0;
    for (int ti = 0; ti < n; ti += S) {
        for (int tj = 0; tj < n; tj += S) {
            std::vector<std::unique_ptr<PE>> pes;
            const int imax = std::min(n, ti + S), jmax = std::min(n, tj + S);
            for (int i = ti; i < imax; ++i)
                for (int j = tj; j < jmax; ++j) {
                    auto pe = std::make_unique<PE>(0, 0, devnull, static_cast<size_t>(n) + 16);
                    pe->setLastRow(true); pe->setLastCol(true);      // boundary: drain, don't forward
                    for (int k = 0; k < n; ++k) {
                        pe->receivedA.push(DataPackage(A[i][k], i, k));   // A element, col = k
                        pe->receivedB.push(DataPackage(B[k][j], k, j));   // B element, row = k
                    }
                    pes.push_back(std::move(pe));
                }
            long cyc = 0; bool any = true;
            while (any) {                                             // lockstep the S x S tile
                any = false;
                for (auto& pe : pes)
                    if (!pe->receivedA.isEmpty()) { pe->cycle(static_cast<uint64_t>(cyc)); any = true; }
                if (any) ++cyc;
            }
            total += cyc;   // output tiles time-share the one physical array
        }
    }
    return total;
}

// Trapezoid (ISCA'24) MS inner-product + multi-fiber intersection baseline, on the REAL
// PE mesh. Output-stationary S x S array: PE(m,n) intersects A's row-m fiber with B's
// col-n fiber over the contraction index k -- the existing merge-join PE IS the fiber
// intersection unit (match on k => MAC, else advance the smaller). Reuses real PEs for
// the TRUE intersection work (MAC + comparator + buffer pops via the global counters);
// the operand distribution (fiber elements delivered to PEs) and the local accumulate
// are counted from the real fibers (router_out/bufwr_out/accum_out). Inner-product
// visits every NON-EMPTY (m,n) pair, so on diagonal-structured H^k most intersections
// are disjoint -- the known inner-product inefficiency Trapezoid's MS mode accepts.
// Returns C (offset->entries) so the result is verifiable against the dense reference.
static ConvDiag trapInnerProduct(const ConvDiag& A, const ConvDiag& B, int n, int S,
                                 long long& cycles_out, long long& router_out,
                                 long long& accum_out, long long& bufwr_out) {
    static std::ofstream devnull;
    // A row-fibers: row m -> sorted (k=col, val).  B col-fibers: col n -> sorted (k=row, val).
    std::unordered_map<int, std::vector<std::pair<int,double>>> Arow, Bcol;
    for (const auto& [o, e] : A) for (const auto& [val, r, c] : e) Arow[r].emplace_back(c, val);
    for (const auto& [o, e] : B) for (const auto& [val, r, c] : e) Bcol[c].emplace_back(r, val);
    for (auto& [m, f] : Arow) std::sort(f.begin(), f.end());
    for (auto& [nn, f] : Bcol) std::sort(f.begin(), f.end());
    std::vector<int> rows, cols;
    for (auto& [m, f] : Arow) rows.push_back(m);
    for (auto& [nn, f] : Bcol) cols.push_back(nn);
    std::sort(rows.begin(), rows.end()); std::sort(cols.begin(), cols.end());

    std::unordered_map<long long, double> Cacc;          // (m<<32|n) -> value
    auto keyOf = [](int m, int nn){ return (static_cast<long long>(m) << 32) | static_cast<unsigned int>(nn); };
    long long cyc = 0, router = 0, accum = 0, bufwr = 0;
    for (size_t ti = 0; ti < rows.size(); ti += S) {     // output-stationary S x S tiles, one at a time
        for (size_t tj = 0; tj < cols.size(); tj += S) {
            std::vector<std::unique_ptr<PE>> pes;
            size_t imax = std::min(rows.size(), ti + S), jmax = std::min(cols.size(), tj + S);
            for (size_t ii = ti; ii < imax; ++ii)
                for (size_t jj = tj; jj < jmax; ++jj) {
                    int m = rows[ii], nn = cols[jj];
                    const auto& af = Arow[m]; const auto& bf = Bcol[nn];
                    auto pe = std::make_unique<PE>(0, 0, devnull, af.size() + bf.size() + 16);
                    pe->setLastRow(true); pe->setLastCol(true);           // boundary: intersect locally
                    for (const auto& [k, v] : af) pe->receivedA.push(DataPackage(v, m, k));    // A[m,k]: index2=k
                    for (const auto& [k, v] : bf) pe->receivedB.push(DataPackage(v, k, nn));   // B[k,n]: index1=k
                    router += static_cast<long long>(af.size() + bf.size());   // fibers delivered to this PE (NoC)
                    bufwr  += static_cast<long long>(af.size() + bf.size());   // ... written into its input buffers
                    pes.push_back(std::move(pe));
                }
            long tile_cyc = 0; bool any = true;
            while (any) {                                  // lockstep the tile's intersections
                any = false;
                for (auto& pe : pes)
                    if (!pe->receivedA.isEmpty() && !pe->receivedB.isEmpty()) { pe->cycle((uint64_t)tile_cyc); any = true; }
                if (any) ++tile_cyc;
            }
            // Trapezoid is a 2D spatial array: each output tile pays systolic fill+drain
            // (operands stream S deep in, results drain S out) -- the same +2S/tile the TPU
            // baseline charges. Without this the inner-product would be unrealistically
            // ideal (pure walk length, PEs MAC-ing from cycle 0).
            if (!pes.empty()) cyc += tile_cyc + 2 * S;

            for (auto& pe : pes)
                while (!pe->PsumOut.isEmpty()) {
                    DataPackage r = pe->PsumOut.front(); pe->PsumOut.pop();
                    Cacc[keyOf(r.index1, r.index2)] += r.value;   // result package = (val, m, n)
                    ++accum;                                       // one local accumulate per product
                }
        }
    }
    cycles_out = cyc; router_out = router; accum_out = accum; bufwr_out = bufwr;
    ConvDiag C;
    for (auto& [key, val] : Cacc) {
        if (val == 0.0) continue;
        int m = static_cast<int>(key >> 32), nn = static_cast<int>(key & 0xffffffffLL);
        C[nn - m].emplace_back(val, m, nn);
    }
    for (auto& [o, e] : C)
        std::sort(e.begin(), e.end(), [](auto& a, auto& b){ return std::get<1>(a) < std::get<1>(b); });
    return C;
}

// Trapezoid (ISCA'24) HS Gustavson dataflow (TrGS) baseline, on the REAL PE mesh.
// Row-wise (expand-merge) SpGEMM: for each C-row m, scale each B-row-k by A[m,k] and
// accumulate -- so it touches ONLY actual products (A-nnz x B-row-nnz), never walking
// disjoint fibers. This is why it beats the inner-product on highly-sparse inputs. The
// expand (B-row fetch + product distribution by output column) is counted truly; the
// products themselves are fed to real PEs as pre-matched pairs (every step is a match =>
// MAC, so compares == MACs, i.e. ZERO intersection waste, vs inner-product's walk). Same
// MAC count as inner-product (both = |{(m,k,n): A[m,k]!=0 & B[k,n]!=0}|), but far fewer
// cycles/compares on sparse. Output-stationary S x S tiles. Returns C for verification.
static ConvDiag trapGustavson(const ConvDiag& A, const ConvDiag& B, int n, int S,
                              long long& cycles_out, long long& router_out,
                              long long& accum_out, long long& bufwr_out) {
    static std::ofstream devnull;
    std::unordered_map<int, std::vector<std::pair<int,double>>> Arow, Brow;   // A: m->(k,val); B: k->(n,val)
    for (const auto& [o, e] : A) for (const auto& [val, r, c] : e) Arow[r].emplace_back(c, val);
    for (const auto& [o, e] : B) for (const auto& [val, r, c] : e) Brow[r].emplace_back(c, val);
    // Expand-merge: build each output (m,n)'s matched products (aval,bval). Only real
    // products are generated -- no empty-pair visits.
    std::map<std::pair<int,int>, std::vector<std::pair<double,double>>> prod;
    long long router = 0, bufwr = 0;
    for (const auto& [m, af] : Arow)
        for (const auto& [k, aval] : af) {
            auto it = Brow.find(k);
            if (it == Brow.end()) continue;
            bufwr += static_cast<long long>(it->second.size());   // stream B-row-k
            for (const auto& [nn, bval] : it->second) {
                prod[{m, nn}].emplace_back(aval, bval);
                router += 1;                                       // product routed to output column
            }
        }
    // Distinct output rows/cols for S x S output-stationary tiling.
    std::set<int> rowset, colset;
    for (const auto& [mn, _] : prod) { rowset.insert(mn.first); colset.insert(mn.second); }
    std::vector<int> rows(rowset.begin(), rowset.end()), cols(colset.begin(), colset.end());
    std::unordered_map<long long, double> Cacc;
    auto keyOf = [](int m, int nn){ return (static_cast<long long>(m) << 32) | static_cast<unsigned int>(nn); };
    long long cyc = 0, accum = 0;
    for (size_t ti = 0; ti < rows.size(); ti += S) {
        for (size_t tj = 0; tj < cols.size(); tj += S) {
            std::vector<std::unique_ptr<PE>> pes;
            size_t imax = std::min(rows.size(), ti + S), jmax = std::min(cols.size(), tj + S);
            for (size_t ii = ti; ii < imax; ++ii)
                for (size_t jj = tj; jj < jmax; ++jj) {
                    auto pit = prod.find({rows[ii], cols[jj]});
                    if (pit == prod.end() || pit->second.empty()) continue;   // no products => no PE work
                    int m = rows[ii], nn = cols[jj];
                    auto pe = std::make_unique<PE>(0, 0, devnull, pit->second.size() + 16);
                    pe->setLastRow(true); pe->setLastCol(true);
                    int key = 0;
                    for (const auto& [aval, bval] : pit->second) {   // pre-matched pairs: every step is a match
                        pe->receivedA.push(DataPackage(aval, m, key));
                        pe->receivedB.push(DataPackage(bval, key, nn));
                        ++key;
                    }
                    pes.push_back(std::move(pe));
                }
            long tile_cyc = 0; bool any = true;
            while (any) {
                any = false;
                for (auto& pe : pes)
                    if (!pe->receivedA.isEmpty() && !pe->receivedB.isEmpty()) { pe->cycle((uint64_t)tile_cyc); any = true; }
                if (any) ++tile_cyc;
            }
            if (!pes.empty()) cyc += tile_cyc + 2 * S;             // 2D array fill/drain per tile
            for (auto& pe : pes)
                while (!pe->PsumOut.isEmpty()) {
                    DataPackage r = pe->PsumOut.front(); pe->PsumOut.pop();
                    Cacc[keyOf(r.index1, r.index2)] += r.value; ++accum;
                }
        }
    }
    cycles_out = cyc; router_out = router; accum_out = accum; bufwr_out = bufwr;
    ConvDiag C;
    for (auto& [key, val] : Cacc) {
        if (val == 0.0) continue;
        int m = static_cast<int>(key >> 32), nn = static_cast<int>(key & 0xffffffffLL);
        C[nn - m].emplace_back(val, m, nn);
    }
    for (auto& [o, e] : C)
        std::sort(e.begin(), e.end(), [](auto& a, auto& b){ return std::get<1>(a) < std::get<1>(b); });
    return C;
}

// Trapezoid MS inner-product on the CONNECTED mesh (runMeshTile): every component --
// including NoC routing and reduction -- is MEASURED through real PE/Connection/reduction
// objects, exactly like the diagonal (ours) and dense (TPU) runs. C[m,n] mapped to PE(i,j):
// X row-m fiber (left, sorted by contraction k) x Y col-n fiber (top, sorted by k). Product
// carries (n,m); C[m][n] read back. Output-stationary S x S tiles + per-tile fill/drain.
static ConvDiag trapInnerProductMesh(const ConvDiag& X, const ConvDiag& Y, int n, int S,
                                     long long& cycles_out) {
    std::unordered_map<int, std::vector<std::pair<int,double>>> Xrow, Ycol;
    for (const auto& [o,e]:X) for (const auto& [v,r,c]:e) Xrow[r].emplace_back(c, v);   // X[m,k]: row m, k=col
    for (const auto& [o,e]:Y) for (const auto& [v,r,c]:e) Ycol[c].emplace_back(r, v);   // Y[k,nn]: k=row, col nn
    for (auto& [m,f]:Xrow) std::sort(f.begin(), f.end());
    for (auto& [nn,f]:Ycol) std::sort(f.begin(), f.end());
    std::vector<int> rows, cols;
    for (auto& [m,f]:Xrow) rows.push_back(m);
    for (auto& [nn,f]:Ycol) cols.push_back(nn);
    std::sort(rows.begin(),rows.end()); std::sort(cols.begin(),cols.end());
    std::map<std::pair<int,int>, double> Cacc;
    long long cyc = 0;
    for (size_t tj = 0; tj < cols.size(); tj += S)                 // A/top = Y column-fibers
        for (size_t ti = 0; ti < rows.size(); ti += S) {          // B/left = X row-fibers
            size_t jmax = std::min(cols.size(), tj + S), imax = std::min(rows.size(), ti + S);
            std::vector<int> A_labels, B_labels;
            std::vector<std::vector<DataPackage>> A_pk, B_pk;
            for (size_t jj = tj; jj < jmax; ++jj) {
                int nn = cols[jj]; A_labels.push_back(nn);
                std::vector<DataPackage> pk;
                for (const auto& [k,v] : Ycol[nn]) pk.emplace_back(v, nn, k);   // (Yval,index1=nn,index2=k), by k
                A_pk.push_back(std::move(pk));
            }
            for (size_t ii = ti; ii < imax; ++ii) {
                int m = rows[ii]; B_labels.push_back(m);
                std::vector<DataPackage> pk;
                for (const auto& [k,v] : Xrow[m]) pk.emplace_back(v, k, m);     // (Xval,index1=k,index2=m), by k
                B_pk.push_back(std::move(pk));
            }
            cyc += runMeshTile(A_labels, B_labels, std::move(A_pk), std::move(B_pk), Cacc) + 2 * S;
        }
    cycles_out = cyc;
    ConvDiag C;
    for (auto& [key,val] : Cacc) {
        if (val == 0.0) continue;
        int nn = key.first, m = key.second;
        C[nn - m].emplace_back(val, m, nn);
    }
    for (auto& [o,e] : C) std::sort(e.begin(), e.end(), [](auto&a, auto&b){ return std::get<1>(a) < std::get<1>(b); });
    return C;
}

// True per-component energy + PIPELINED cycle breakdown from the MEASURED activity
// counters (PE:: and DiagonalReduction::). Every count comes from the real cycle-
// accurate datapath. The cycle model is an OVERLAPPED pipeline: each stage (compute,
// buffer, router, reduction) is throughput-bound on its own units, the stages overlap
// so steady-state = the slowest stage, and the reduction is a pipelined adder tree
// (1 sum/cycle/lane, log2(rows) depth). A fill/drain latency is added once. So the
// non-compute stages genuinely consume cycles and can gate the array (not free).
static void reportBreakdown(const std::string& dataflow, int grid_row, int grid_col,
                            long long base_cycles, long long hbm_bytes,
                            const std::string& csv_path,
                            long long add_router = 0, long long add_accum = 0,
                            long long add_bufwr = 0) {
    // ---- measured counts (add_* let a self-contained isolated-PE dataflow supply the
    // distribution/accumulate traffic it performs outside the connected NoC/reduction) ----
    const uint64_t mac   = PE::macCount();
    const uint64_t comp  = PE::compareCount();
    const uint64_t rout  = PE::routerCount() + add_router;
    const uint64_t bwr   = PE::bufWriteCount() + add_bufwr;
    const uint64_t brd   = PE::bufReadCount();
    const uint64_t accum = DiagonalReduction::accumulationCount() + add_accum;
    const long long PEs  = static_cast<long long>(grid_row) * grid_col;

    // ---- pipelined, overlapped cycle model (tunable hardware resources) ----
    const int BUF_PORTS = 2;                 // operand-buffer accesses served per PE/cycle
    const int LINKS_PE  = 2;                 // NoC out-links per PE (bottom + right)
    const long long redux_lanes = std::max(1, grid_col);   // one pipelined adder tree per column
    auto cdiv = [](double a, double b){ return b > 0 ? std::ceil(a / b) : 0.0; };
    const double mac_cyc   = static_cast<double>(base_cycles);                 // measured compute makespan
    const double buf_cyc   = cdiv(static_cast<double>(bwr + brd), static_cast<double>(PEs) * BUF_PORTS);
    const double rout_cyc  = cdiv(static_cast<double>(rout),      static_cast<double>(PEs) * LINKS_PE);
    const double redux_cyc = cdiv(static_cast<double>(accum),     static_cast<double>(redux_lanes));
    const double steady    = std::max(std::max(mac_cyc, buf_cyc), std::max(rout_cyc, redux_cyc));
    const double tree_depth= std::ceil(std::log2(std::max(2, grid_row)));      // adder-tree depth
    const double filldrain = static_cast<double>(grid_row + grid_col) + tree_depth;  // systolic fill + tree drain
    const double real_cyc  = steady + filldrain;
    const char* bottleneck = (steady==mac_cyc)?"compute":(steady==redux_cyc)?"reduction":
                             (steady==buf_cyc)?"buffer":(steady==rout_cyc)?"router":"compute";

    // ---- energy split (per-op weights normalized to a MAC; Horowitz/Eyeriss-class) ----
    const double E_MAC=1.0, E_BUF=1.0, E_ROUTER=2.0, E_ACCUM=1.0, E_MEM=200.0;
    const double mem_acc = static_cast<double>(hbm_bytes) / 32.0;              // HBM burst word
    const double e_mac=E_MAC*mac, e_buf=E_BUF*(bwr+brd), e_rout=E_ROUTER*rout,
                 e_acc=E_ACCUM*accum, e_mem=E_MEM*mem_acc;
    const double e_tot = e_mac+e_buf+e_rout+e_acc+e_mem;
    auto pct = [&](double e){ return e_tot > 0 ? 100.0*e/e_tot : 0.0; };
    // PE utilization against the REAL (pipeline-bound) cycle count.
    const double util = (PEs>0 && real_cyc>0) ? 100.0*static_cast<double>(mac)/(static_cast<double>(PEs)*real_cyc) : 0.0;
    // cycle-share of each stage (of the real total): the bottleneck's steady share plus its slice of overlap.
    auto cyc_pct = [&](double c){ return real_cyc>0 ? 100.0*c/real_cyc : 0.0; };

    std::cout << std::fixed << std::setprecision(2)
        << "[breakdown " << dataflow << "] real cycles " << static_cast<long long>(real_cyc)
        << " (compute " << base_cycles << ", bottleneck=" << bottleneck << ")  PE-util " << util << "%\n"
        << "  cycle stages: compute " << mac_cyc << " buffer " << buf_cyc << " router " << rout_cyc
        << " reduction " << redux_cyc << " fill/drain " << filldrain << "\n"
        << "  energy%: MAC " << pct(e_mac) << " MEM " << pct(e_mem) << " ROUTER " << pct(e_rout)
        << " BUFFER " << pct(e_buf) << " ACCUM " << pct(e_acc)
        << "  | counts mac=" << mac << " cmp=" << comp << " router=" << rout
        << " buf=" << (brd+bwr) << " accum=" << accum << "\n";
    std::cout.unsetf(std::ios::floatfield); std::cout << std::setprecision(6);
    if (!csv_path.empty()) {
        const std::string bp = csv_path + ".breakdown.csv";
        std::ifstream chk(bp); bool empty = !chk.good() || chk.peek() == std::ifstream::traits_type::eof();
        chk.close();
        std::ofstream bd(bp, std::ios::app);
        if (bd) {
            if (empty) bd << "dataflow,grid_pes,compute_cycles,real_cycles,bottleneck,pe_util_pct,"
                             "mac_cyc,buf_cyc,router_cyc,redux_cyc,filldrain_cyc,"
                             "mac_pct,mem_pct,router_pct,buffer_pct,accum_pct,"
                             "mac,compare,router,buf_write,buf_read,accum,hbm_bytes\n";
            bd << dataflow << ',' << PEs << ',' << base_cycles << ',' << static_cast<long long>(real_cyc) << ','
               << bottleneck << ',' << std::fixed << std::setprecision(4) << util << ','
               << std::setprecision(1) << mac_cyc << ',' << buf_cyc << ',' << rout_cyc << ',' << redux_cyc << ',' << filldrain << ','
               << std::setprecision(3) << pct(e_mac) << ',' << pct(e_mem) << ',' << pct(e_rout) << ',' << pct(e_buf) << ',' << pct(e_acc) << ','
               << mac << ',' << comp << ',' << rout << ',' << bwr << ',' << brd << ',' << accum << ',' << hbm_bytes << '\n';
        }
    }
}

int main(int argc, char* argv[]) {
    int total_cycles = 0;
    int max_abs_offset = 0;           // widest diagonal offset seen -> drives edge-scratchpad depth
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
    // Dataflow + optimization ablation (see docs/pe-utilization-offset-convolution.md).
    //   -dataflow=merge  : current index-comparison merge-join mesh (default)
    //   -dataflow=conv   : offset-space convolution (aligned MAC; no index search)
    //   -zeroskip=1      : spend a cycle only on nonzero products (conv only)
    //   -balance=1       : segment long diagonals across idle PEs (conv makespan)
    //   -hermitian=1     : store/stream only offsets >= 0 (H, H^j are Hermitian) -> ~2x less traffic
    //   -fused=1         : keep running Taylor sum + current power on-chip (no per-power HBM round-trip)
    const std::string dataflow = args.count("dataflow") ? args["dataflow"] : "merge";
    const bool zeroskip = args.count("zeroskip") ? std::stoi(args["zeroskip"]) != 0 : false;
    const bool cbalance = args.count("cbalance") ? std::stoi(args["cbalance"]) != 0 : false;  // conv far-diagonal balancing
    const bool hermitian= args.count("hermitian")? std::stoi(args["hermitian"])!= 0 : false;
    const bool fused    = args.count("fused")    ? std::stoi(args["fused"])    != 0 : false;
    const int  pe_budget= args.count("pe")       ? std::stoi(args["pe"])       : 0;   // 0 = one PE per diagonal pair; >0 caps PE count (equal-PE compare)

    // Zero the true activity counters so the breakdown reflects only this run.
    PE::resetActivity();
    DiagonalReduction::resetAccumulation();

    // -dense: the TPU-like no-sparsity baseline measured on the SAME instrumented mesh.
    // Replaces H with a fully dense matrix (every diagonal populated) so the connected
    // mesh processes the dense multiply -- identical code path, counters, and pipelined
    // cycle model as the sparse run, differing only in input density. Small n only
    // (the dense mesh is (2n-1) diagonals => (2n-1)^2 PEs).
    const bool dense_baseline = args.count("dense") ? std::stoi(args["dense"]) != 0 : false;
    // Modeled per-PE input FIFO: a small STATIC pipeline buffer (a hardware
    // constant, reported only). The large operand buffering / merge-join alignment
    // is the job of the edge scratchpad (CScratchpad), which must hold ~O(max
    // diagonal offset) entries -- see the reported max_offset / scratchpad sizing.
    const size_t pe_fifo_entries = args.count("fifo") ? static_cast<size_t>(std::stoul(args["fifo"])) : 16;
    bool verify_all_pass = true;   // AND of every -verify iteration (true if -verify off)

    // The simulator's per-PE operand buffer is backed by the scratchpad, so size it
    // generously (>= longest diagonal it may stage) to avoid a false overflow; it is
    // not the modeled hardware FIFO. Peak occupancy is reported to size the scratchpad.
    const size_t sim_fifo_cap = std::max<size_t>(2ULL * static_cast<size_t>(size), 100000ULL);
    PE::resetPeakOccupancy();
    PE::setPeakTracking(true);

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
    // Auto-detect the input format: a DIA file (hamlib/dia_oom/*.txt) starts with
    // "N <n> D <d>"; the legacy sparse files are "(row,col): value" lines.
    bool is_dia = false;
    {
        std::ifstream pk(filenameA);
        std::string l0;
        if (pk && std::getline(pk, l0) && l0.rfind("N ", 0) == 0 && l0.find(" D ") != std::string::npos)
            is_dia = true;
    }
    std::vector<int> A_offsets;
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> current_diag;
    if (is_dia) {
        long n = loadDiaMatrix(filenameA, A_offsets, current_diag);
        if (n <= 0 || A_offsets.empty()) {
            std::cerr << "Fatal: bad/empty DIA file " << filenameA << "\n";
            return 2;
        }
        size = static_cast<int>(n);                 // trust the file's dimension
        qubit_size = 0; while ((1L << qubit_size) < n) ++qubit_size;
        std::cout << "DIA input: n=" << n << " (q=" << qubit_size << "), "
                  << A_offsets.size() << " diagonals\n";
    } else {
        A_offsets = extractDiagonalOffsets(filenameA);
        // Fail loudly on a missing/empty input. Otherwise the run silently produces
        // an empty result and -verify "passes" (empty vs empty) — which would let a
        // bad manifest path masquerade as a valid sweep row.
        if (A_offsets.empty()) {
            std::cerr << "Fatal: no diagonals read from " << filenameA
                      << " (missing/empty/unreadable file)\n";
            return 2;
        }
        current_diag = createDiagonalMap(filenameA, A_offsets, size);
    }

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

    // -dense: densify H (all 2n-1 diagonals, fully populated) so the connected mesh
    // runs the no-sparsity multiply for the same-methodology TPU breakdown. Small n only.
    if (dense_baseline) {
        if (size > 128) {
            std::cerr << "[dense] size " << size << " > 128: dense mesh is (2n-1)^2 PEs (infeasible)\n";
            return 3;
        }
        current_diag.clear();
        A_offsets.clear();
        for (int o = -(size - 1); o <= size - 1; ++o) {
            std::vector<std::tuple<double,int,int>> ents;
            const int len = size - std::abs(o);
            for (int k = 0; k < len; ++k) {
                int row = o >= 0 ? k : k - o;
                int col = o >= 0 ? k + o : k;
                ents.emplace_back(1.0, row, col);      // values irrelevant to activity/cycle counts
            }
            A_offsets.push_back(o);
            current_diag[o] = std::move(ents);
        }
        std::sort(A_offsets.begin(), A_offsets.end());
        std::cout << "[dense] TPU-like baseline: densified to " << A_offsets.size()
                  << " diagonals (n=" << size << ") on the same instrumented mesh\n";
    }

    // ---- TPU-like dense systolic MXU baseline (equal PE count) --------------------
    // A weight-stationary S x S matrix-multiply unit sized so that S^2 == our mesh's
    // PE count (grid_row * grid_col) -> an EQUAL-PE comparison. Unlike the sparse
    // diagonal dataflows it cannot exploit sparsity: it computes each Taylor power as
    // a full DENSE n x n GEMM (H^{k} = H^{k-1} @ H). Cycles and HBM traffic are modeled
    // analytically -- streaming n^3 MACs cycle-by-cycle is infeasible -- but the same
    // Ramulator HBM4 backend (streamBytes) charges the dense operand/result traffic, so
    // the memory model is identical to the sparse runs. This is the classic dense
    // baseline: near-100% array utilization but astronomically more MACs, quantifying
    // exactly what the diagonal/sparse dataflow saves.  (Cycle count fits int64 for
    // q <= ~24, which covers the sweep range.)
    if (dataflow == "tpu" || dataflow == "tpugrid") {
        const double kGHz = 1.0;
        const long long n = size;                              // matrix dim = 2^qubit
        const long long pe_count = static_cast<long long>(grid_row) * grid_col;
        const long long S = std::max(1LL, static_cast<long long>(std::llround(std::sqrt(static_cast<double>(pe_count)))));
        const long long tiles = (n + S - 1) / S;               // S-sized tiles per dimension
        // Weight-stationary GEMM: for each of tiles*tiles weight tiles, fill the array
        // (S cyc), stream all n activation rows (n cyc), drain (S cyc). One dense GEMM
        // per Taylor power (iterations of them). cycles/GEMM = tiles^2 * (n + 2S).
        const long long cyc_analytic = tiles * tiles * (n + 2 * S);
        long long cyc_per_mm = cyc_analytic;
        const int kTpuGridMax = 512;                            // dense real-grid sim is n^3/S^2 cyc
        if (dataflow == "tpugrid") {
            if (n <= kTpuGridMax) {
                DenseMat dH(static_cast<size_t>(n), std::vector<double>(static_cast<size_t>(n), 0.0));
                fillDense(dH, current_diag, static_cast<int>(n));
                cyc_per_mm = tpuOnGrid(dH, dH, static_cast<int>(n), static_cast<int>(S));   // REAL PE mesh
                std::cout << "[tpugrid] real PE mesh = " << cyc_per_mm
                          << " cyc/GEMM  (analytic model = " << cyc_analytic
                          << ", streaming term tiles^2*n = " << tiles * tiles * n
                          << ")  n=" << n << " S=" << S << "\n";
            } else {
                std::cout << "[tpugrid] n=" << n << " > " << kTpuGridMax
                          << ": dense real-grid sim is n^3/S^2 cyc (infeasible); using analytic model\n";
            }
        }
        const long long total_cyc  = cyc_per_mm * static_cast<long long>(iterations);
        const double    mac_work   = static_cast<double>(n) * n * n * iterations;  // n^3 per GEMM
        // Dense HBM traffic (TPU-favorable lower bound): each GEMM reads both n x n
        // operands once and writes the n x n result once (8 B/elem). A real weight-
        // stationary MXU re-reads activations ~n/S x more, so charging each element
        // once UNDERSTATES the baseline's memory cost -- conservative for our claim.
        const long long bytes_per_mat = n * n * static_cast<long long>(sizeof(double));
        for (int it = 0; it < iterations; ++it) {
            hbmMemory.streamBytes(static_cast<size_t>(bytes_per_mat), /*isWrite=*/false); // read prev power
            hbmMemory.streamBytes(static_cast<size_t>(bytes_per_mat), /*isWrite=*/false); // read H
            hbmMemory.streamBytes(static_cast<size_t>(bytes_per_mat), /*isWrite=*/true);  // write new power
        }
        max_abs_offset = static_cast<int>(n - 1);   // dense => full band (fits int for q<=30)
        HBMMemory::Stats st = hbmMemory.getStats();
        const double compute_ns = static_cast<double>(total_cyc) / kGHz;
        const double dram_ns    = static_cast<double>(st.cycles);
        const double hidden_ns  = std::min(compute_ns, dram_ns);      // double-buffered overlap
        const double exposed_ns = dram_ns - hidden_ns;
        const double runtime_ns = compute_ns + exposed_ns;
        const double mem_pct    = runtime_ns > 0 ? 100.0 * exposed_ns / runtime_ns : 0.0;
        const double hbm_tCK    = hbmMemory.tCK();
        const uint64_t hbm_dram_cycles = hbm_tCK > 0 ? static_cast<uint64_t>(dram_ns / hbm_tCK) : 0;
        const long long spm_weight_kib = S * S * static_cast<long long>(sizeof(double)) / 1024; // stationary weight tile
        std::cout << std::fixed << std::setprecision(2)
                  << "[tpu] MXU " << S << "x" << S << " (=" << pe_count << " PEs), n=" << n
                  << " | dense GEMMs " << iterations << " x " << cyc_per_mm << " = " << total_cyc
                  << " cyc, MAC work " << mac_work
                  << ", HBM " << (st.bytesRead + st.bytesWritten) / (1024.0 * 1024) << " MiB, mem-latency "
                  << mem_pct << " %\n";
        std::cout.unsetf(std::ios::floatfield);
        if (!csv_path.empty()) {
            std::ofstream csv(csv_path, std::ios::app);
            if (csv)
                csv << output_name << ',' << qubit_size << ',' << grid_row << ',' << grid_col << ','
                    << iterations << ',' << (reuse?1:0) << ',' << (ctile?1:0) << ',' << (balance?1:0) << ','
                    << total_cyc << ',' << 0 << ',' << 0 << ',' << max_abs_offset << ','
                    << st.cycles << ',' << hbm_dram_cycles << ','
                    << st.bytesRead << ',' << st.bytesWritten << ','
                    << std::fixed << std::setprecision(3) << mem_pct << ','
                    << std::setprecision(0) << spm_weight_kib << ','
                    << 0 << ',' << 0 << ','
                    << "n/a"
                    << ',' << dataflow << ',' << 0 << ',' << 0 << ',' << 0 << ',' << 0 << '\n';
        }
        return 0;
    }

    // ---- Trapezoid (ISCA'24) MS inner-product + intersection baseline (equal-PE) -----
    // Reimplemented from the paper (no public code). Output-stationary S x S array,
    // S^2 == our mesh PE count, same HBM4 backend, same breakdown model. Runs on the
    // REAL PE mesh (small n) and VERIFIES its result against the dense reference.
    if (dataflow == "trapezoid" || dataflow == "trapezoid_hs") {
        const bool hs = (dataflow == "trapezoid_hs");        // HS: Gustavson (TrGS); else MS: inner-product
        const long long S = std::max(1LL, static_cast<long long>(std::llround(std::sqrt(static_cast<double>(grid_row) * grid_col))));
        if (size > 1024) { std::cerr << "[trapezoid] n=" << size << " > 1024: real-mesh sim infeasible\n"; return 3; }
        const ConvDiag H_diag = current_diag;
        auto bytesOf = [&](const ConvDiag& M){ long long b=0; for (const auto& [o,e]:M) b += static_cast<long long>(e.size())*sizeof(DataPackage); return b; };
        long long total_cyc=0, total_router=0, total_accum=0, total_bufwr=0;
        hbmMemory.streamBytes(static_cast<size_t>(bytesOf(H_diag)), false);   // read H once
        for (int it = 0; it < iterations; ++it) {
            long long cyc=0, rout=0, acc=0, bwr=0;
            ConvDiag C;
            if (hs) C = trapGustavson(current_diag, H_diag, size, static_cast<int>(S), cyc, rout, acc, bwr);
            else    C = trapInnerProductMesh(current_diag, H_diag, size, static_cast<int>(S), cyc);  // CONNECTED mesh: all measured
            total_cyc+=cyc; total_router+=rout; total_accum+=acc; total_bufwr+=bwr;
            for (const auto& [o,e]:C) max_abs_offset = std::max(max_abs_offset, std::abs(o));
            hbmMemory.streamBytes(static_cast<size_t>(bytesOf(current_diag)), false);  // read prev power
            hbmMemory.streamBytes(static_cast<size_t>(bytesOf(H_diag)), false);        // re-read H
            hbmMemory.streamBytes(static_cast<size_t>(bytesOf(C)), true);              // write new power
            if (verify) {
                ref = denseMatMul(ref, denseH, size);
                DenseMat sim(size, std::vector<double>(size, 0.0));
                fillDense(sim, C, size);
                double md=0.0; long mm=0;
                for (int i=0;i<size;++i) for (int j=0;j<size;++j){ double d=std::abs(sim[i][j]-ref[i][j]); if(d>md)md=d; if(d>1e-6)++mm; }
                std::cout << "[verify] iter " << it << ": " << (mm?"FAIL":"PASS")
                          << " (max_abs_diff=" << md << ", mismatches=" << mm << ")\n";
                if (mm) verify_all_pass = false;
            }
            current_diag = std::move(C);
        }
        total_cycles = static_cast<int>(total_cyc);
        HBMMemory::Stats st = hbmMemory.getStats();
        std::cout << "[trapezoid] " << (hs ? "HS Gustavson(TrGS)" : "MS inner-product(TrIP)")
                  << " S=" << S << "x" << S << " (=" << (grid_row*grid_col)
                  << " PEs) | cycles " << total_cyc << ", MACs " << PE::macCount()
                  << ", compares " << PE::compareCount() << "\n";
        reportBreakdown(hs ? "trapezoid_hs" : "trapezoid", grid_row, grid_col, total_cyc,
                        static_cast<long long>(st.bytesRead + st.bytesWritten), csv_path,
                        total_router, total_accum, total_bufwr);
        return 0;
    }

    // ---- Offset-space convolution dataflow (self-contained; bypasses the mesh) ----
    // Computes S's powers H^1..H^{iterations} as diagonal convolutions (aligned MACs,
    // no index search), models compute cycles analytically under the levers, and
    // charges HBM traffic (Hermitian halves it; fused avoids per-power round-trips).
    if (dataflow == "conv" || dataflow == "convgrid") {
        const bool grid_conv = (dataflow == "convgrid");   // cycle-accurate on real PEs
        const double kGHz = 1.0;
        const ConvDiag H_diag = current_diag;    // B = H, reused across powers
        auto bytesOf = [&](const ConvDiag& M) {
            long long b = 0;
            for (const auto& [o, e] : M) {
                if (hermitian && o < 0) continue;   // Hermitian: keep offsets >= 0 only
                b += static_cast<long long>(e.size()) * static_cast<long long>(sizeof(DataPackage));
            }
            return b;
        };
        long long total_cyc = 0;
        long long total_work = 0;                         // total MAC work (for equal-PE compare)
        const long long kSpmBudget = 2LL * 1024 * 1024;   // 2 MB edge scratchpad
        long long peak_footprint = 0;                     // peak on-chip bytes (report as spm_peak)
        long long conv_spill_events = 0;                  // # powers whose footprint spilled
        for (const auto& [o, e] : current_diag) max_abs_offset = std::max(max_abs_offset, std::abs(o));
        hbmMemory.streamBytes(static_cast<size_t>(bytesOf(H_diag)), /*isWrite=*/false);  // read H once
        for (int it = 0; it < iterations; ++it) {
            long ms = 0; long long nnz = 0; int moff = 0;
            ConvDiag C;
            long work = 0;
            if (grid_conv) {                 // cycle-accurate on the real PE array
                C = convOnGrid(current_diag, H_diag, size, zeroskip, ms);
                for (const auto& [o, e] : C) moff = std::max(moff, std::abs(o));
            } else {                          // fast analytic model
                C = convMatmul(current_diag, H_diag, size, zeroskip, cbalance, ms, nnz, moff, work, pe_budget);
            }
            total_cyc += ms; total_work += work;
            max_abs_offset = std::max(max_abs_offset, moff);
            // on-chip footprint = resident H + current power (+ prev during ping-pong)
            const long long foot = bytesOf(H_diag) + bytesOf(C);
            peak_footprint = std::max(peak_footprint, foot);
            if (!fused) {
                hbmMemory.streamBytes(static_cast<size_t>(bytesOf(current_diag)), false); // read prev power
                hbmMemory.streamBytes(static_cast<size_t>(bytesOf(H_diag)), false);       // re-read H
                hbmMemory.streamBytes(static_cast<size_t>(bytesOf(C)), true);             // write new power
            } else {
                // fused: H is read once (above) and reused; each power stays on-chip up to
                // the scratchpad budget, only the overflow spills. An intermediate power's
                // overflow is written out and read back for the next matmul; the final power
                // is written out fully (it leaves the chip). Always <= the non-fused round
                // trip, which additionally re-reads H every power.
                const long long powerBytes = bytesOf(C);
                const long long overflow = ctile ? std::max(0LL, powerBytes - kSpmBudget) : 0;
                if (it + 1 < iterations) {                 // intermediate power (reused as next operand)
                    if (overflow > 0) {
                        hbmMemory.streamBytes(static_cast<size_t>(overflow), true);   // spill overflow out
                        hbmMemory.streamBytes(static_cast<size_t>(overflow), false);  // read back next matmul
                        ++conv_spill_events;
                    }
                } else {                                    // final power leaves the chip
                    hbmMemory.streamBytes(static_cast<size_t>(powerBytes), true);
                }
            }
            if (verify) {
                ref = denseMatMul(ref, denseH, size);
                DenseMat sim(size, std::vector<double>(size, 0.0));
                fillDense(sim, C, size);
                double md = 0.0; long mm = 0;
                for (int i = 0; i < size; ++i)
                    for (int j = 0; j < size; ++j) {
                        double d = std::abs(sim[i][j] - ref[i][j]);
                        if (d > md) md = d;
                        if (d > 1e-6) ++mm;
                    }
                std::cout << "[verify] iter " << it << ": " << (mm ? "FAIL" : "PASS")
                          << " (max_abs_diff=" << md << ", mismatches=" << mm << ")\n";
                if (mm) verify_all_pass = false;
            }
            current_diag = std::move(C);
        }
        total_cycles = static_cast<int>(total_cyc);
        HBMMemory::Stats st = hbmMemory.getStats();
        const double compute_ns = static_cast<double>(total_cyc) / kGHz;
        const double dram_ns    = static_cast<double>(st.cycles);
        const double hidden_ns  = std::min(compute_ns, dram_ns);     // double-buffered overlap
        const double exposed_ns = dram_ns - hidden_ns;
        const double runtime_ns = compute_ns + exposed_ns;
        const double mem_pct    = runtime_ns > 0 ? 100.0 * exposed_ns / runtime_ns : 0.0;
        const double hbm_tCK    = hbmMemory.tCK();
        const uint64_t hbm_dram_cycles = hbm_tCK > 0 ? static_cast<uint64_t>(dram_ns / hbm_tCK) : 0;
        std::cout << std::fixed << std::setprecision(2)
                  << "[conv] zeroskip=" << zeroskip << " balance=" << cbalance
                  << " hermitian=" << hermitian << " fused=" << fused
                  << " pe=" << pe_budget
                  << " | compute " << total_cyc << " cyc, MAC work " << total_work
                  << ", HBM " << (st.bytesRead + st.bytesWritten) / (1024.0 * 1024) << " MiB, mem-latency "
                  << mem_pct << " %, max_offset " << max_abs_offset << "\n";
        std::cout.unsetf(std::ios::floatfield);
        if (!csv_path.empty()) {
            std::ofstream csv(csv_path, std::ios::app);
            if (csv)
                csv << output_name << ',' << qubit_size << ',' << grid_row << ',' << grid_col << ','
                    << iterations << ',' << (reuse?1:0) << ',' << (ctile?1:0) << ',' << (balance?1:0) << ','
                    << total_cycles << ',' << pe_fifo_entries << ',' << 0 << ',' << max_abs_offset << ','
                    << st.cycles << ',' << hbm_dram_cycles << ','
                    << st.bytesRead << ',' << st.bytesWritten << ','
                    << std::fixed << std::setprecision(3) << mem_pct << ','
                    << std::setprecision(0) << (peak_footprint / 1024.0) << ','
                    << conv_spill_events << ',' << conv_spill_events << ','
                    << (verify ? (verify_all_pass ? "PASS" : "FAIL") : "n/a")
                    << ',' << dataflow << ',' << (zeroskip?1:0) << ',' << (cbalance?1:0)
                    << ',' << (hermitian?1:0) << ',' << (fused?1:0) << '\n';
        }
        return total_cycles;
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
        // Track the widest diagonal offset streamed (A, B, and the growing C): the
        // edge scratchpad must buffer ~this many entries to align the merge-join.
        for (int o : A_offsets)  max_abs_offset = std::max(max_abs_offset, std::abs(o));
        for (int o : B_offsets)  max_abs_offset = std::max(max_abs_offset, std::abs(o));
        for (int o : C_offsets)  max_abs_offset = std::max(max_abs_offset, std::abs(o));
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
                    RunResult r = run_test_case(A_offsets_local, B_offsets_local, A_diag_copy, B_diag_copy,
                                           scheduler, C_base, C_offset_to_group, C_scratch, out, Energyout,
                                           sim_fifo_cap, /*commit=*/true);
                    cycles = r.cycles;
                    total_cycles += r.cycles;
                    row_compute_ns += static_cast<double>(r.cycles) / kAccelClockGHz;
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
                    RunResult r = run_test_case(A_offsets_local, B_offsets_local, A_diag_local, B_diag_local,
                                           scheduler, C_base, C_offset_to_group, C_scratch, out, Energyout,
                                           sim_fifo_cap, /*commit=*/true);
                    cycles = r.cycles;
                    total_cycles += r.cycles;
                    double t = static_cast<double>(r.cycles) / kAccelClockGHz;
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
    // True per-component + pipelined-cycle breakdown of the connected-mesh run. Same
    // physical array (grid_row x grid_col) for sparse (ours) and dense (TPU baseline):
    // dense just maps more diagonal tiles onto it -> more cycles, same methodology.
    reportBreakdown(dense_baseline ? "tpu_dense" : dataflow, grid_row, grid_col,
                    static_cast<long long>(total_cycles),
                    static_cast<long long>(totalStats.bytesRead + totalStats.bytesWritten),
                    csv_path);
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
    // Per-PE FIFO is a small static pipeline buffer; the alignment buffering lives
    // in the edge scratchpad, which must hold ~max_abs_offset operands per stream.
    const uint64_t peak_occ = PE::peakOccupancy();
    std::cout << "Per-PE static FIFO: " << pe_fifo_entries << " entries ("
              << (pe_fifo_entries * sizeof(DataPackage)) << " B).  "
              << "Edge scratchpad staging: peak occupancy " << peak_occ
              << " entries, max diagonal offset " << max_abs_offset
              << " (=> ~" << (static_cast<size_t>(max_abs_offset) * sizeof(DataPackage) / 1024)
              << " KiB/stream to align the merge-join)\n";
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
                << total_cycles << ','        // compute cycles
                << pe_fifo_entries << ','     // per-PE static FIFO depth (entries, hw constant)
                << peak_occ << ','            // peak per-PE occupancy = scratchpad staging need
                << max_abs_offset << ','      // widest diagonal offset (drives scratchpad depth)
                << mem_cycles << ','          // HBM time (ns)
                << hbm_dram_cycles << ','     // HBM time (DRAM cycles)
                << totalStats.bytesRead << ',' << totalStats.bytesWritten << ','
                << std::fixed << std::setprecision(3) << mem_latency_pct << ','
                << std::setprecision(0) << (scratchpad_peak_bytes / 1024.0) << ','
                << c_spill_reads << ',' << c_spill_writes << ','
                << (verify ? (verify_all_pass ? "PASS" : "FAIL") : "n/a")
                << ',' << dataflow << ',' << (zeroskip?1:0) << ',' << (cbalance?1:0)
                << ',' << (hermitian?1:0) << ',' << (fused?1:0) << '\n';
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
