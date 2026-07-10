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

// ConvDiag: diagonal map (offset -> row-sorted nonzero (val,row,col)). The analytic
// convolution kernel (convMatmul) now lives in the separate analytical/ driver
// (analytical/HBMHamiltonianAnalytic.cpp). This is the cycle-accurate driver: it keeps
// only the real-mesh convOnGrid path below.
using ConvDiag = std::unordered_map<int, std::vector<std::tuple<double,int,int>>>;

// Cycle-accurate convolution ON the real PE array with a FIXED S*S PE budget.
//
// Each valid (dA,dB) offset pair contributes a contiguous run of OFFSET-ALIGNED products
// (A row k paired with B row k+dA). We materialize the active products (post-zeroskip),
// assign them to the S*S PEs, then run the real merge-join PE::cycle() lockstep. Because
// A.col == B.row on every fed head, each product MACs in one cycle, so a PE's cycle count
// equals its queue length and the simulated makespan = the busiest PE (cycle-accurate, not
// a formula).
//
// Two levers, both realized at this cycle-accurate level:
//   L2 zeroskip : a zero product (a*b==0) never enters a PE queue -> fewer real MAC cycles.
//   L3 cbalance : split long diagonals into equal contiguous chunks across ALL S*S PEs
//                 (flexible-NoC distribution, SIGMA/Flexagon class) so no single PE owns the
//                 length-n main diagonal. Without it, whole diagonals stay on one PE and the
//                 longest one bounds the makespan.
//
// Router accounting (the flexible-NoC OVERHEAD that cbalance introduces): the base rigid
// dataflow reduces each output diagonal LOCALLY (~0 extra hops). Splitting/spreading work
// forces operand segments out to non-native PEs and partials back to per-diagonal
// collectors. We charge, per emitted product, the Manhattan distance dist(p)=r+c of its
// assigned PE from the corner collector (a per-flit NoC path length). base packs the big
// diagonals onto low-dist PEs (near 0 extra hops); cbalance spreads across the whole mesh
// (high hops) -- so router_out is exactly cbalance's routing cost, to be weighted in the
// downstream (Verilog) energy model. accum_out = one accumulate per product.
static ConvDiag convOnGrid(const ConvDiag& A, const ConvDiag& B, int n, int S,
                           bool zeroskip, bool cbalance, long& cycles_out,
                           long long& router_out, long long& accum_out,
                           long long& nnz_out, int& maxoff_out, int reduce_lanes = 0) {
    static std::ofstream devnull;   // unopened: PE's out<< become cheap no-ops
    const int P = std::max(1, S * S);
    auto toVec = [&](const ConvDiag& M) {
        std::unordered_map<int, std::vector<double>> v;
        for (const auto& [o, e] : M) { auto& a = v[o]; a.assign(n, 0.0); for (const auto& [val, r, c] : e) a[r] = val; }
        return v;
    };
    auto Av = toVec(A), Bv = toVec(B);

    // 1) Flatten the active products; record per-pair spans so base can keep diagonals whole.
    struct PRef { int dA, dB, k; };
    std::vector<PRef> flat;
    int maxoff = 0;
    for (const auto& [dA, va] : Av) {
        for (const auto& [dB, vb] : Bv) {
            const int dC = dA + dB;
            const int lo = std::max(0, std::max(-dA, -dC));
            const int hi = std::min(n, std::min(n - dA, n - dC));
            if (hi <= lo) continue;
            const long long start = (long long)flat.size();
            for (int k = lo; k < hi; ++k) {
                if (zeroskip && va[k] * vb[k + dA] == 0.0) continue;
                flat.push_back({dA, dB, k});
            }
            if ((long long)flat.size() > start) maxoff = std::max(maxoff, std::abs(dC));
        }
    }
    const long long total = (long long)flat.size();
    maxoff_out = maxoff; nnz_out = total;

    // 2) Assign products to PEs.
    //   base (rigid systolic): each OUTPUT diagonal dC is reduced on ONE fixed PE (dC mod P),
    //     matching the per-PE offset-space DiagonalReduction. The longest output diagonal is a
    //     real hotspot -> it bounds the makespan (no load balancing).
    //   cbalance (flexible NoC): equal contiguous chunks over ALL P PEs, splitting the long
    //     diagonal across idle PEs (breaks dC-locality, which is why it costs gather routing).
    std::vector<std::vector<long long>> peq(P);          // indices into flat
    if (cbalance) {
        const long long chunk = (total + P - 1) / P;
        for (long long i = 0; i < total; ++i)
            peq[std::min<long long>(P - 1, chunk ? i / chunk : 0)].push_back(i);
    } else {
        for (long long i = 0; i < total; ++i) {
            int dC = flat[i].dA + flat[i].dB;
            peq[((dC % P) + P) % P].push_back(i);
        }
    }

    // 3) Router / accum counts + cross-PE gather load.
    //   dist(p)  = Manhattan hops of PE p from the corner collector (per-flit path length).
    //   home(dC) = the PE that OWNS output diagonal dC's reduction (the base mapping). A
    //     partial computed on a PE other than home(dC) cannot be accumulated locally: it
    //     must be routed to home(dC) and summed in the reduction network. base keeps every
    //     dC on its home PE (crossPE == 0 -> no gather); cbalance spreads dC across the mesh
    //     to shorten the compute makespan, and pays for it here in routed+reduced partials.
    auto dist = [S](int p){ return (long long)(p / S) + (long long)(p % S); };
    auto home = [P](int dC){ return ((dC % P) + P) % P; };
    long long router = 0, crossPE = 0;
    for (int p = 0; p < P; ++p) {
        router += dist(p) * (long long)peq[p].size();                          // per-flit path length
        for (long long idx : peq[p]) {
            const int dC = flat[idx].dA + flat[idx].dB;
            if (home(dC) != p) ++crossPE;      // partial lives off its collector -> network reduce
        }
    }
    router_out = router;
    accum_out  = total;                                                        // one accumulate / product

    // 4) Cycle-accurate run: feed each PE its products, MAC lockstep, makespan = busiest PE.
    std::unordered_map<int, std::unordered_map<int, double>> acc;   // dC -> row -> value
    auto drain = [&](PE& pe) {
        while (!pe.PsumOut.isEmpty()) {
            DataPackage p = pe.PsumOut.front(); pe.PsumOut.pop();
            acc[p.index2 - p.index1][p.index1] += p.value;          // dC = col - row
        }
    };
    std::vector<std::unique_ptr<PE>> pes;
    for (int p = 0; p < P; ++p) {
        if (peq[p].empty()) continue;
        auto pe = std::make_unique<PE>(0, 0, devnull, peq[p].size() + 16);
        pe->setLastRow(true); pe->setLastCol(true);   // boundary: drain, don't forward
        for (long long idx : peq[p]) {
            const PRef& pr = flat[idx];
            double a = Av[pr.dA][pr.k], b = Bv[pr.dB][pr.k + pr.dA];
            pe->receivedA.push(DataPackage(a, pr.k, pr.k + pr.dA));                     // A: row k, col k+dA
            pe->receivedB.push(DataPackage(b, pr.k + pr.dA, pr.k + pr.dA + pr.dB));     // B: row k+dA, col k+dC
        }
        pes.push_back(std::move(pe));
    }
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

    // Flexible-NoC gather cost -- the cycles cbalance's routing ACTUALLY costs, which the
    // pure compute makespan (cyc = busiest PE's MAC queue) omitted entirely.
    //
    // OVERLAP model (systolic / pipelined): scatter -> MAC -> gather+reduce are pipeline
    // STAGES, so the steady-state makespan is the BOTTLENECK stage + a one-time fill/drain
    // latency, NOT the sum. compute runs at P MACs/cyc; the reduction network sums the
    // crossPE partials at kReduceLanes adds/cyc and OVERLAPS compute -- it adds cycles only
    // when it cannot keep up (gather_thr > compute). With a P-wide network gather_thr =
    // crossPE/P <= total/P = compute, so routing hides behind compute and its ONLY cycle
    // cost is the deeper pipeline's transport + tree-depth fill/drain (its dominant cost is
    // then ENERGY = router_out, weighed downstream in Verilog). A NARROWER network
    // (kReduceLanes < P) makes the gather stage the bottleneck and adds real cycles. base
    // keeps every diagonal on its home PE (crossPE == 0) -> makespan unchanged.
    //
    // ASSUMPTION (tunable via -reduce_lanes): reduce_lanes<=0 defaults to a well-provisioned
    // P-wide reduction network (SIGMA/Flexagon class, ~one adder per PE); a smaller value
    // narrows the network so the gather stage bottlenecks. crossPE is a conservative UPPER
    // bound (every off-home partial, not the post-local-sum residual), so it errs toward more.
    const long kReduceLanes = reduce_lanes > 0 ? std::min(reduce_lanes, P) : P;   // adds/cycle
    if (crossPE > 0) {
        const long gather_thr  = static_cast<long>((crossPE + kReduceLanes - 1) / kReduceLanes);
        const long netDiameter = 2 * (S - 1);           // transport fill+drain (one-time latency)
        long treeDepth = 0; for (int m = P; m > 1; m = (m + 1) / 2) ++treeDepth;   // ceil(log2 P)
        cyc = std::max(cyc, gather_thr) + netDiameter + treeDepth;   // bottleneck stage + fill/drain
    }
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
    //   -dataflow=merge    : index-comparison merge-join mesh (default)
    //   -dataflow=convgrid : offset-space convolution, CYCLE-ACCURATE on the real S*S mesh
    //   -dataflow=trapezoid[_hs] : ISCA'24 Trapezoid baseline on the real mesh
    //   (the analytic -dataflow=conv / tpu models moved to analytical/HBMHamiltonianAnalytic.cpp)
    //   -zeroskip=1      : spend a cycle only on nonzero products (convgrid only)
    //   -cbalance=1      : split long diagonals across the S*S PEs (convgrid makespan)
    //   -hermitian=1     : store/stream only offsets >= 0 (H, H^j are Hermitian) -> ~2x less traffic
    //   -fused=1         : keep running Taylor sum + current power on-chip (no per-power HBM round-trip)
    // convgrid (offset-space DIA convolution on the real S*S mesh) is the ONLY dataflow
    // this driver runs; the merge-join / trapezoid / analytic paths were removed (baselines
    // live in accel_compare; analytic models in analytical/). Kept as a label for the CSV.
    const std::string dataflow = "convgrid";
    const bool zeroskip = args.count("zeroskip") ? std::stoi(args["zeroskip"]) != 0 : false;
    const bool cbalance = args.count("cbalance") ? std::stoi(args["cbalance"]) != 0 : false;  // conv far-diagonal balancing
    const bool hermitian= args.count("hermitian")? std::stoi(args["hermitian"])!= 0 : false;
    const bool fused    = args.count("fused")    ? std::stoi(args["fused"])    != 0 : false;
    const int  pe_budget= args.count("pe")       ? std::stoi(args["pe"])       : 0;   // 0 = one PE per diagonal pair; >0 caps PE count (equal-PE compare)
    // Reduction-network width for the convgrid cbalance gather stage (adds/cycle). 0 (default)
    // = well-provisioned P = S*S (one adder per PE): routing overlaps compute, costing only
    // fill/drain latency. A smaller N makes the gather stage the bottleneck and charges
    // cbalance real cycles -- sweep it to expose cbalance's sensitivity to NoC provisioning.
    const int  reduce_lanes = args.count("reduce_lanes") ? std::stoi(args["reduce_lanes"]) : 0;

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


    // ---- Offset-space convolution dataflow, CYCLE-ACCURATE on the real S*S PE mesh ----
    // Computes the powers H^1..H^{iterations} as diagonal convolutions (aligned MACs,
    // no index search) on the connected PE array, and charges HBM traffic (Hermitian
    // halves it; fused avoids per-power round-trips). The analytic makespan model
    // (convMatmul, -dataflow=conv) lives in the separate analytical/ driver.
    {   // ---- convOnGrid: the top compute function (offset-space convolution) ----
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
        long long total_router = 0, total_accum = 0;      // cycle-accurate op counts (grid path)
        const long long kSpmBudget = 2LL * 1024 * 1024;   // 2 MB edge scratchpad
        long long peak_footprint = 0;                     // peak on-chip bytes (report as spm_peak)
        long long conv_spill_events = 0;                  // # powers whose footprint spilled
        for (const auto& [o, e] : current_diag) max_abs_offset = std::max(max_abs_offset, std::abs(o));
        hbmMemory.streamBytes(static_cast<size_t>(bytesOf(H_diag)), /*isWrite=*/false);  // read H once
        for (int it = 0; it < iterations; ++it) {
            long ms = 0; long long nnz = 0; int moff = 0;
            ConvDiag C;
            long work = 0;
            long long rt = 0, ac = 0;        // cycle-accurate on the real PE array (S*S budget)
            C = convOnGrid(current_diag, H_diag, size, grid_row, zeroskip, cbalance, ms, rt, ac, nnz, moff, reduce_lanes);
            total_router += rt; total_accum += ac; work = static_cast<long>(nnz);
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
        // Cycle-accurate op counts for the (Verilog) energy model. mac/compare are measured
        // in the real PE datapath; router/accum are the flexible-NoC counts from convOnGrid.
        const long long mac_ops     = static_cast<long long>(PE::macCount());
        const long long compare_ops = static_cast<long long>(PE::compareCount());
        std::cout << std::fixed << std::setprecision(2)
                  << "[conv] zeroskip=" << zeroskip << " cbalance=" << cbalance
                  << " pe=" << (grid_row * grid_col)
                  << " | compute " << total_cyc << " cyc"
                  << " | ops: mac=" << mac_ops << " cmp=" << compare_ops
                  << " router=" << total_router << " accum=" << total_accum
                  << " | HBM " << (st.bytesRead + st.bytesWritten) / (1024.0 * 1024) << " MiB, mem-latency "
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
                    << ',' << (hermitian?1:0) << ',' << (fused?1:0)
                    << ',' << mac_ops << ',' << compare_ops << ',' << total_router << ',' << total_accum << '\n';
        }
        // Return 0 (success), NOT total_cycles: the process exit code is (total_cycles % 256),
        // so returning the cycle count makes the shell see a nonzero status and (in the sweep)
        // fire the "|| TIMEOUT" fallback on a run that actually succeeded -- polluting the CSV
        // with spurious TIMEOUT rows. Every other dataflow path already returns 0.
        return 0;
    }
}
