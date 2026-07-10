#pragma once
#include <cstdint>
#include <vector>
#include <tuple>
#include <unordered_map>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include "../include/AcceleratorHBM.h"

// Geometry that matches your simulator
namespace HBMDiagIO {
static constexpr uint32_t kChannels      = 8;
static constexpr uint32_t kBGs           = 4;   // bank-groups per channel
static constexpr uint32_t kBanksPerBG    = 4;   // banks per BG
static constexpr uint32_t kBurstBytes    = 64;  // 64 B per burst
static constexpr uint32_t kRowBytes      = 2048;// 2 KiB row buffer
static constexpr uint32_t kBurstsPerRow  = kRowBytes / kBurstBytes; // 32
static constexpr uint32_t kChanSelLowBit = 6;   // channel bits for 64B interleaving
static constexpr uint32_t kChanSelBits   = 3;

// Compose device address from (channel, bg, bankIn, rowIdx, colBurst)
static inline uint64_t compose_addr(uint32_t ch, uint32_t bg, uint32_t bankIn,
                                    uint64_t rowIdx, uint32_t colBurst)
{
    uint64_t addr = 0, off = 0;
    addr |= (0ull & ((1ull<<6)-1)) << off; off += 6;
    addr |= (uint64_t(colBurst) & ((1ull<<5)-1)) << off; off += 5;
    addr |= (uint64_t(bankIn)   & ((1ull<<2)-1)) << off; off += 2;
    addr |= (uint64_t(bg)       & ((1ull<<2)-1)) << off; off += 2;
    addr |= (rowIdx << off);

    const uint64_t mask = ((1ull<<kChanSelBits)-1) << kChanSelLowBit;
    addr = (addr & ~mask) | (uint64_t(ch & ((1u<<kChanSelBits)-1)) << kChanSelLowBit);
    return addr;
}

// Packs doubles into 64B bursts and writes them to HBM with optimal layout
inline void writeDiagonalsAndReportBandwidth(
    AcceleratorHBM::HBMController& hbm,
    const std::unordered_map<int, std::vector<std::tuple<double,int,int>>>& diagMap,
    int matrixSize,
    uint32_t preferBankInGroup = 0,
    uint64_t baseRowStart = 1)
{
    using AcceleratorHBM::MemRequest;

    // // Safety: ensure HBM is initialized
    // if (hbm.getTotalMemorySize() == 0) {
    //     uint64_t neededSize = 4ULL * 1024 * 1024 * 1024; // default 4 GiB
    //     std::cout << "[HBM] Initializing memory to " << (neededSize >> 30) << " GiB...\n";
    //     hbm.init(neededSize);
    //     hbm.startup(0);
    // }

    // Flatten diagonals
    std::vector<int> offsets;
    offsets.reserve(diagMap.size());
    for (auto& kv : diagMap) offsets.push_back(kv.first);
    std::sort(offsets.begin(), offsets.end());

    std::vector<std::vector<double>> diagValues;
    diagValues.reserve(offsets.size());
    for (int d : offsets) {
        const auto& entries = diagMap.at(d);
        std::vector<std::pair<int,double>> tmp;
        tmp.reserve(entries.size());
        for (auto& t : entries) {
            double v; int i,j;
            std::tie(v,i,j) = t;
            tmp.emplace_back(i, v);
        }
        std::sort(tmp.begin(), tmp.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });
        std::vector<double> vals; vals.reserve(tmp.size());
        for (auto& p : tmp) vals.push_back(p.second);
        diagValues.push_back(std::move(vals));
    }

    const uint64_t arrival = 0;
    uint32_t rrCh = 0, rrBG = 0;
    const uint32_t bankIn = preferBankInGroup % kBanksPerBG;
    uint64_t totalBytes = 0;
    alignas(64) std::array<uint8_t, kBurstBytes> burst{};

    uint64_t rowIdx[kChannels][kBGs];
    for (uint32_t c=0;c<kChannels;++c)
        for (uint32_t g=0;g<kBGs;++g)
            rowIdx[c][g] = baseRowStart;

    // Progress bar setup
    size_t totalBursts = 0;
    for (auto& vals : diagValues)
        totalBursts += (vals.size() + 7) / 8;
    size_t burstsDone = 0;

    auto printProgress = [&](size_t done) {
        const int barWidth = 50;
        double progress = double(done) / totalBursts;
        int pos = int(barWidth * progress);
        std::cout << "\r[";
        for (int i = 0; i < barWidth; ++i)
            std::cout << (i < pos ? "#" : (i == pos ? ">" : "-"));
        std::cout << "] " << std::setw(5) << std::fixed << std::setprecision(1)
                  << (progress * 100.0) << "%";
        std::cout.flush();
    };

    std::cout << "\n[HBM Write] Streaming " << offsets.size()
              << " diagonals (" << totalBursts << " bursts)...\n";
    std::cout << "[DEBUG] Limiting to first 3 diagonals for debugging\n";
    printProgress(0);

    // Main write loop - LIMIT TO FIRST 3 DIAGONALS FOR DEBUG
    size_t maxDiags = std::min(size_t(3), offsets.size());
    for (size_t di = 0; di < maxDiags; ++di) {
        const auto& vals = diagValues[di];
        uint32_t ch = rrCh;
        uint32_t bg = rrBG;
        uint64_t row = rowIdx[ch][bg];
        uint32_t colBurst = 0;
        size_t pos = 0;

        std::cout << "\n[DEBUG] Processing diagonal " << di << "/" << maxDiags
                  << " with " << vals.size() << " values, ch=" << ch << " bg=" << bg << std::endl;

        while (pos < vals.size()) {
            uint32_t doublesInThis = 0;
            while (doublesInThis < 8 && pos < vals.size()) {
                std::memcpy(burst.data() + doublesInThis*8, &vals[pos], 8);
                ++doublesInThis; ++pos;
            }
            for (; doublesInThis < 8; ++doublesInThis) {
                uint64_t zero = 0;
                std::memcpy(burst.data() + doublesInThis*8, &zero, 8);
            }

            uint64_t addr = compose_addr(ch, bg, bankIn, row, colBurst);
            
            if (burstsDone < 3) {
                std::cout << "[DEBUG Burst " << burstsDone << "] "
                          << "ch=" << ch << " bg=" << bg << " bankIn=" << bankIn
                          << " row=" << row << " colBurst=" << colBurst
                          << " -> addr=0x" << std::hex << addr << std::dec << std::endl;
            }
            
            MemRequest req{};
            req.addr        = addr;
            req.size        = kBurstBytes;
            req.isRead      = false;
            req.isWrite     = true;
            req.arrivalTime = arrival;
            req.data        = const_cast<uint8_t*>(burst.data());
            hbm.recvTimingReq(&req, arrival);
            totalBytes += kBurstBytes;

            burstsDone++;
            if (burstsDone % 512 == 0 || burstsDone == totalBursts)
                printProgress(burstsDone);

            colBurst++;
            if (colBurst == kBurstsPerRow) {
                colBurst = 0;
                row++;
            }
        }

        rowIdx[ch][bg] = row;
        rrBG = (rrBG + 1) % kBGs;
        if (rrBG == 0) rrCh = (rrCh + 1) % kChannels;
    }

    printProgress(totalBursts);
    std::cout << "  ✓ Done issuing writes\n";
    std::cout << "Write Queue Size after writes: "
               << hbm.writeQueueSize() << "\n";
    
    // Track how many requests were issued (totalBursts is the actual count)
    uint64_t totalIssued = totalBursts;
    std::cout << "Total requests issued: " << totalIssued << "\n";
    
    // Capture stats before drain
    auto statsBefore = hbm.getStats();
    uint64_t completedBefore = statsBefore.readReqs + statsBefore.writeReqs;
    
    // Drain the simulation to idle and measure time (with progress bar)
    uint64_t sim = 0;
    uint32_t iter = 0, maxIter = 50'000'000;
    uint32_t lastShown = 0;

    // Helper: print drain progress
    auto printDrain = [&](uint64_t ns, double pct, uint64_t pending) {
        const int barWidth = 50;
        int pos = int(barWidth * pct);
        std::cout << "\r[HBM Drain] [";
        for (int i = 0; i < barWidth; ++i)
            std::cout << (i < pos ? "#" : (i == pos ? ">" : "-"));
        std::cout << "] " << std::setw(5) << std::fixed << std::setprecision(1)
                << (pct * 100.0) << "%  sim=" << std::setw(8) << ns
                << " ns  pending=" << pending << std::flush;
    };

    while (hbm.hasOutstandingRequests() && iter < maxIter) {
        for (uint32_t ch = 0; ch < kChannels; ++ch) {
            hbm.processNextReqEvent(ch, sim);
            hbm.processRespondEvent(ch, sim);
        }
        sim += 1; iter++;

        // Refresh progress bar occasionally
        if (iter % 1000 == 0) {  // Changed from 10000 to 1000 for more frequent updates
            uint64_t completedNow = hbm.getStats().readReqs + hbm.getStats().writeReqs;
            uint64_t done = completedNow - completedBefore;
            uint64_t pending = totalIssued > done ? (totalIssued - done) : 0;
            double pct = std::min(1.0, double(done) / totalIssued);
            printDrain(sim, pct, pending);
            lastShown = iter;
        }
        
        // Force break after 100k iterations for debug
        if (iter >= 100000) {
            std::cout << "\n[DEBUG] Breaking after 100k iterations for debugging\n";
            break;
        }
    }
    
    uint64_t completedFinal = hbm.getStats().readReqs + hbm.getStats().writeReqs;
    uint64_t actualCompleted = completedFinal - completedBefore;
    printDrain(sim, 1.0, 0);
    std::cout << "  ✓ Done draining (" << actualCompleted << " requests completed)\n";
    if (iter >= maxIter)
    std::cerr << "[HBM Drain] Reached maxIter (" << maxIter
              << ") — possible stuck outstanding requests!\n";

    if (sim == 0) sim = 1; // avoid division by zero

    double gibps = (double)totalBytes * 1e9 / (sim * 1024.0 * 1024.0 * 1024.0);
    auto& stats = hbm.getStats();

    std::cout << "\n=== Diagonal Write Summary ===\n";
    std::cout << "Total diagonals: " << offsets.size() << "\n";
    std::cout << "Total bytes written: " << totalBytes << " B\n";
    std::cout << "Simulation time: " << sim << " ns\n";
    std::cout << "Write bandwidth: " << std::fixed << std::setprecision(2)
              << gibps << " GiB/s\n";
    std::cout << "Per-channel accesses: [";
    for (uint32_t ch = 0; ch < kChannels; ++ch) {
        std::cout << stats.channelAccesses[ch] << (ch < kChannels-1 ? ", " : "");
    }
    std::cout << "]\n";
}
} // namespace HBMDiagIO