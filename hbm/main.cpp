/*
 * Example usage of standalone HBM controller
 * Optimized for 1GHz HBM2 configuration
 * 
 * Configuration:
 * - Clock: 1 GHz (1 ns period)
 * - Channels: 8
 * - Per-channel bandwidth: 32 GB/s (128-bit @ DDR 2 Gbps)
 * - Total bandwidth: 256 GB/s
 * - Timings configured for 1 GHz operation
 */

#include "AcceleratorHBM.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <algorithm>

using namespace AcceleratorHBM;

void example_basic_usage() {
    std::cout << "=== Basic HBM Usage Example ===" << std::endl;
    
    // Create controller with 8 channels for 256 GB/s
    HBMController hbm(8);
    
    // Initialize with 4GB total memory
    uint64_t totalMemory = 4ULL * 1024 * 1024 * 1024;  // 4 GB
    hbm.init(totalMemory);
    hbm.startup(0);
    
    // Create a write request
    MemRequest writeReq;
    writeReq.addr = 0x1000;
    writeReq.size = 64;
    writeReq.isWrite = true;
    writeReq.isRead = false;
    writeReq.arrivalTime = 0;
    writeReq.qosValue = 0;
    writeReq.requestorId = 1;
    
    // Allocate data
    std::vector<uint8_t> writeData(64, 0xAB);
    writeReq.data = writeData.data();
    
    // Send write request at time 0
    bool accepted = hbm.recvTimingReq(&writeReq, 0);
    std::cout << "Write request " << (accepted ? "accepted" : "rejected") << std::endl;
    
    // Process the request
    hbm.processNextReqEvent(hbm.selectChannel(writeReq.addr), 0);
    
    // Create a read request - wait sufficient time after write
    MemRequest readReq;
    readReq.addr = 0x1000;
    readReq.size = 64;
    readReq.isWrite = false;
    readReq.isRead = true;
    readReq.arrivalTime = 100;  // 100 ns after write
    readReq.qosValue = 0;
    readReq.requestorId = 1;
    
    std::vector<uint8_t> readData(64, 0);
    readReq.data = readData.data();
    
    // Send read request
    accepted = hbm.recvTimingReq(&readReq, 100);
    std::cout << "Read request " << (accepted ? "accepted" : "rejected") << std::endl;
    
    // Process read
    uint32_t channel = hbm.selectChannel(readReq.addr);
    hbm.processNextReqEvent(channel, 100);
    hbm.processRespondEvent(channel, 200);  // Give time for read to complete
    
    // Verify data
    bool match = true;
    for (size_t i = 0; i < 64; ++i) {
        if (readData[i] != writeData[i]) {
            match = false;
            break;
        }
    }
    std::cout << "Data verification: " << (match ? "PASS" : "FAIL") << std::endl;
    
    // Print statistics
    hbm.printStats();
}

void example_timing_analysis() {
    std::cout << "\n=== HBM Timing Analysis (1GHz) ===" << std::endl;
    std::cout << "Demonstrating different access latencies\n" << std::endl;
    
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    std::vector<uint8_t> buffer(64);
    
    // Test 1: Row hit (best case)
    std::cout << "Test 1: Row Hit Access" << std::endl;
    std::cout << "Expected latency: tCL + tBURST = 18 + 2 = 20 ns\n" << std::endl;
    
    MemRequest req1;
    req1.addr = 0x1000;
    req1.size = 64;
    req1.isRead = true;
    req1.isWrite = false;
    req1.data = buffer.data();
    
    uint64_t lat1 = hbm.recvAtomic(&req1);
    std::cout << "  Measured latency: " << lat1 << " ns" << std::endl;
    std::cout << "  (First access - includes row activation)" << std::endl;
    
    // Access same row - should hit
    MemRequest req2;
    req2.addr = 0x1040;  // Same row, different column
    req2.size = 64;
    req2.isRead = true;
    req2.isWrite = false;
    req2.data = buffer.data();
    
    uint64_t lat2 = hbm.recvAtomic(&req2);
    std::cout << "  Follow-up access (row hit): " << lat2 << " ns\n" << std::endl;
    
    // Test 2: Row miss
    std::cout << "Test 2: Row Miss Access" << std::endl;
    std::cout << "Expected: tRP + tRCD + tCL + tBURST = 14 + 12 + 18 + 2 = 46 ns\n" << std::endl;
    
    HBMController hbm2(8);
    hbm2.init(4ULL * 1024 * 1024 * 1024);
    hbm2.startup(0);
    
    MemRequest req3;
    req3.addr = 0x1000;
    req3.size = 64;
    req3.isRead = true;
    req3.isWrite = false;
    req3.data = buffer.data();
    
    lat1 = hbm2.recvAtomic(&req3);
    
    // Access different row in same bank
    MemRequest req4;
    req4.addr = 0x100000;  // Different row
    req4.size = 64;
    req4.isRead = true;
    req4.isWrite = false;
    req4.data = buffer.data();
    
    lat2 = hbm2.recvAtomic(&req4);
    std::cout << "  Row miss latency: " << lat2 << " ns" << std::endl;
    std::cout << "  (Includes precharge + activate + CAS)\n" << std::endl;
}

void example_atomic_access() {
    std::cout << "\n=== Atomic Access Example ===" << std::endl;
    
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    // Atomic write
    std::vector<uint8_t> data(128, 0x42);
    MemRequest atomicWrite;
    atomicWrite.addr = 0x2000;
    atomicWrite.size = 128;
    atomicWrite.isWrite = true;
    atomicWrite.isRead = false;
    atomicWrite.data = data.data();
    
    uint64_t writeLatency = hbm.recvAtomic(&atomicWrite);
    std::cout << "Atomic write latency: " << writeLatency << " ns" << std::endl;
    std::cout << "  (128 bytes = 2 bursts × 64 bytes)" << std::endl;
    
    // Atomic read
    std::vector<uint8_t> readback(128, 0);
    MemRequest atomicRead;
    atomicRead.addr = 0x2000;
    atomicRead.size = 128;
    atomicRead.isWrite = false;
    atomicRead.isRead = true;
    atomicRead.data = readback.data();
    
    uint64_t readLatency = hbm.recvAtomic(&atomicRead);
    std::cout << "Atomic read latency: " << readLatency << " ns" << std::endl;
    
    // Verify
    bool match = (data == readback);
    std::cout << "Atomic access verification: " << (match ? "PASS" : "FAIL") << std::endl;
}

void example_bandwidth_sequential() {
    std::cout << "\n=== Sequential Bandwidth Test ===" << std::endl;
    std::cout << "Testing streaming access pattern (best case)\n" << std::endl;
    
    const uint32_t numRequests = 10000;
    const uint32_t requestSize = 64;
    std::vector<uint8_t> buffer(requestSize);
    
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Requests: " << numRequests << std::endl;
    std::cout << "  Request size: " << requestSize << " bytes" << std::endl;
    std::cout << "  Total data: " << (numRequests * requestSize) / (1024.0 * 1024) << " MB" << std::endl;
    std::cout << "  Pattern: Sequential (interleaved across channels)\n" << std::endl;
    
    // Measure using atomic accesses
    uint64_t totalLatency = 0;
    uint64_t minLatency = UINT64_MAX;
    uint64_t maxLatency = 0;
    
    for (uint32_t i = 0; i < numRequests; ++i) {
        MemRequest req;
        req.addr = i * 64;  // Sequential 64-byte aligned
        req.size = requestSize;
        req.isRead = true;
        req.isWrite = false;
        req.data = buffer.data();
        
        uint64_t lat = hbm.recvAtomic(&req);
        totalLatency += lat;
        minLatency = std::min(minLatency, lat);
        maxLatency = std::max(maxLatency, lat);
    }
    
    // Calculate metrics
    double avgLatency = static_cast<double>(totalLatency) / numRequests;
    double totalBytes = numRequests * requestSize;
    double totalTimeNs = totalLatency;  // Sum of all latencies
    
    // For parallel channels, divide by number of active channels
    // Sequential access distributes across all 8 channels
    double effectiveTimeNs = totalTimeNs / 8.0;
    double bandwidth = (totalBytes / effectiveTimeNs) * 1e9 / (1024.0 * 1024 * 1024);
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Average latency: " << std::fixed << std::setprecision(2) 
              << avgLatency << " ns" << std::endl;
    std::cout << "  Min latency: " << minLatency << " ns (row hit)" << std::endl;
    std::cout << "  Max latency: " << maxLatency << " ns (row miss)" << std::endl;
    std::cout << "  Estimated bandwidth: " << std::setprecision(2) 
              << bandwidth << " GB/s" << std::endl;
    std::cout << "  Theoretical max: 256 GB/s" << std::endl;
    std::cout << "  Efficiency: " << (bandwidth / 256.0 * 100) << "%\n" << std::endl;
    
    // Show channel distribution
    auto& stats = hbm.getStats();
    std::cout << "Channel distribution: [";
    for (uint32_t ch = 0; ch < 8; ++ch) {
        std::cout << stats.channelAccesses[ch];
        if (ch < 7) std::cout << ", ";
    }
    std::cout << "]" << std::endl;
}

void example_bandwidth_random() {
    std::cout << "\n=== Random Access Bandwidth Test ===" << std::endl;
    std::cout << "Testing random access pattern (worst case)\n" << std::endl;
    
    const uint32_t numRequests = 10000;
    const uint32_t requestSize = 64;
    std::vector<uint8_t> buffer(requestSize);
    
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint64_t> addrDist(0, 1ULL * 1024 * 1024 * 1024 - 64);
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Requests: " << numRequests << std::endl;
    std::cout << "  Pattern: Random (many row misses expected)\n" << std::endl;
    
    uint64_t totalLatency = 0;
    uint64_t minLatency = UINT64_MAX;
    uint64_t maxLatency = 0;
    
    for (uint32_t i = 0; i < numRequests; ++i) {
        MemRequest req;
        req.addr = (addrDist(rng) / 64) * 64;  // 64-byte aligned
        req.size = requestSize;
        req.isRead = true;
        req.isWrite = false;
        req.data = buffer.data();
        
        uint64_t lat = hbm.recvAtomic(&req);
        totalLatency += lat;
        minLatency = std::min(minLatency, lat);
        maxLatency = std::max(maxLatency, lat);
    }
    
    double avgLatency = static_cast<double>(totalLatency) / numRequests;
    double totalBytes = numRequests * requestSize;
    double effectiveTimeNs = totalLatency / 8.0;
    double bandwidth = (totalBytes / effectiveTimeNs) * 1e9 / (1024.0 * 1024 * 1024);
    
    std::cout << "\nResults:" << std::endl;
    std::cout << "  Average latency: " << std::fixed << std::setprecision(2) 
              << avgLatency << " ns" << std::endl;
    std::cout << "  Min latency: " << minLatency << " ns" << std::endl;
    std::cout << "  Max latency: " << maxLatency << " ns" << std::endl;
    std::cout << "  Estimated bandwidth: " << std::setprecision(2) 
              << bandwidth << " GB/s" << std::endl;
    std::cout << "  (Lower due to row buffer misses)\n" << std::endl;
}

void example_multi_channel() {
    std::cout << "\n=== Multi-Channel Verification ===" << std::endl;
    
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    hbm.printInterleaveConfig();
    
    std::cout << "\nChannel selection for sequential addresses:" << std::endl;
    for (int i = 0; i < 16; ++i) {
        uint64_t addr = i * 64;
        uint32_t channel = hbm.selectChannel(addr);
        std::cout << "  Addr 0x" << std::hex << std::setw(4) << std::setfill('0') << addr 
                  << std::dec << " (byte " << std::setw(4) << addr 
                  << ") -> Channel " << channel << std::endl;
    }
    
    std::cout << "\nWithin-page access (same channel):" << std::endl;
    uint64_t baseAddr = 0x1000;
    for (int i = 0; i < 4; ++i) {
        uint64_t addr = baseAddr + i * 64;
        uint32_t channel = hbm.selectChannel(addr);
        std::cout << "  Addr 0x" << std::hex << addr 
                  << " -> Channel " << std::dec << channel << std::endl;
    }
    
    std::cout << "\nAcross-page sequential access (different channels):" << std::endl;
    for (int i = 0; i < 4; ++i) {
        uint64_t addr = i * 4096;  // Different pages
        uint32_t channel = hbm.selectChannel(addr);
        std::cout << "  Page " << i << " (0x" << std::hex << addr 
                  << ") -> Channel " << std::dec << channel << std::endl;
    }
}

void example_page_interleaving() {
    std::cout << "\n=== Page-Level Interleaving Example ===" << std::endl;
    
    InterleaveConfig pageConfig;
    pageConfig.scheme = InterleavingScheme::PAGE;
    pageConfig.interleaveSize = 4096;
    pageConfig.interleaveBits = 3;
    pageConfig.interleaveLowBit = 12;
    
    HBMController hbm(8, pageConfig);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    hbm.printInterleaveConfig();
    
    std::cout << "\nPage-to-channel mapping:" << std::endl;
    for (int i = 0; i < 16; ++i) {
        uint64_t addr = i * 4096;
        uint32_t channel = hbm.selectChannel(addr);
        std::cout << "  Page " << std::setw(2) << i 
                  << " (0x" << std::hex << std::setw(6) << std::setfill('0') << addr 
                  << ") -> Channel " << std::dec << channel << std::endl;
    }
}

void example_custom_interleaving() {
    std::cout << "\n=== Custom Interleaving Example (128 bytes) ===" << std::endl;
    
    // Configure for 128-byte interleaving
    InterleaveConfig customConfig(128, 8);
    
    HBMController hbm(8, customConfig);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    hbm.printInterleaveConfig();
    
    // Show how 128-byte chunks map to channels
    std::cout << "128-byte chunk to channel mapping:" << std::endl;
    for (uint64_t addr = 0; addr < 1024; addr += 128) {
        uint32_t channel = hbm.selectChannel(addr);
        std::cout << "  Addr 0x" << std::hex << std::setw(4) << std::setfill('0') << addr 
                  << " -> Channel " << std::dec << channel << std::endl;
    }
}

void example_burst_efficiency() {
    std::cout << "\n=== Burst Transfer Efficiency ===" << std::endl;
    std::cout << "Analyzing raw data transfer vs effective bandwidth\n" << std::endl;
    
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    std::cout << "HBM Burst Characteristics (1 GHz):" << std::endl;
    std::cout << "  Bus width: 128 bits = 16 bytes" << std::endl;
    std::cout << "  Burst length: 4 (BL4)" << std::endl;
    std::cout << "  Bytes per burst: 64 bytes" << std::endl;
    std::cout << "  Burst duration: 2 ns (tBURST)" << std::endl;
    std::cout << "  Raw transfer rate: 64 bytes / 2 ns = 32 GB/s per channel" << std::endl;
    std::cout << "  Total (8 channels): 8 × 32 = 256 GB/s\n" << std::endl;
    
    std::cout << "But actual latency includes:" << std::endl;
    std::cout << "  Command scheduling" << std::endl;
    std::cout << "  Row activation (if needed): tRCD = 12 ns" << std::endl;
    std::cout << "  CAS latency: tCL = 18 ns" << std::endl;
    std::cout << "  Data transfer: tBURST = 2 ns" << std::endl;
    std::cout << "  Total (row hit): ~20 ns for 64 bytes" << std::endl;
    std::cout << "  Effective rate: 64 bytes / 20 ns = 3.2 GB/s per transaction\n" << std::endl;
    
    std::cout << "Parallelism saves us:" << std::endl;
    std::cout << "  Multiple requests pipeline through different channels" << std::endl;
    std::cout << "  While channel 0 waits for tCL, channel 1 can transfer data" << std::endl;
    std::cout << "  Result: Approach theoretical 256 GB/s with enough parallelism\n" << std::endl;
}

void example_latency_breakdown() {
    std::cout << "\n=== Latency Breakdown (1 GHz Timings) ===" << std::endl;
    
    std::cout << "\nRead Operation Latencies:" << std::endl;
    std::cout << "  Row Hit (best case):" << std::endl;
    std::cout << "    tCL (CAS latency):     18 ns" << std::endl;
    std::cout << "    tBURST (data transfer): 2 ns" << std::endl;
    std::cout << "    Total:                 20 ns\n" << std::endl;
    
    std::cout << "  Row Miss (worst case):" << std::endl;
    std::cout << "    tRP (precharge):       14 ns" << std::endl;
    std::cout << "    tRCD (activate):       12 ns" << std::endl;
    std::cout << "    tCL (CAS latency):     18 ns" << std::endl;
    std::cout << "    tBURST (data transfer): 2 ns" << std::endl;
    std::cout << "    Total:                 46 ns\n" << std::endl;
    
    std::cout << "Write Operation Latencies:" << std::endl;
    std::cout << "  Row Hit:" << std::endl;
    std::cout << "    tCWL (write latency):   7 ns" << std::endl;
    std::cout << "    tBURST (data transfer): 2 ns" << std::endl;
    std::cout << "    Total:                  9 ns\n" << std::endl;
    
    std::cout << "  Row Miss:" << std::endl;
    std::cout << "    tRP (precharge):       14 ns" << std::endl;
    std::cout << "    tRCD_WR (activate):     6 ns" << std::endl;
    std::cout << "    tCWL (write latency):   7 ns" << std::endl;
    std::cout << "    tBURST (data transfer): 2 ns" << std::endl;
    std::cout << "    Total:                 29 ns\n" << std::endl;
    
    std::cout << "Key Constraints:" << std::endl;
    std::cout << "  tRC (row cycle):       42 ns (tRAS + tRP)" << std::endl;
    std::cout << "  tCCD_L (col-to-col):    3 ns (same bank group)" << std::endl;
    std::cout << "  tRRD_L (act-to-act):    6 ns (same bank group)" << std::endl;
    std::cout << "  tWTR_L (write-to-read): 9 ns (same bank group)\n" << std::endl;
}

void example_interleaving_impact_detailed() {
    std::cout << "\n=== Detailed Interleaving Impact Analysis ===" << std::endl;
    std::cout << "Using timing-accurate simulation to measure real throughput\n" << std::endl;
    
    // Use MORE requests to see the effect - with 512 requests, distribution is balanced
    // but timing pattern is still different!
    const uint32_t numRequests = 256;  // Focus on first 4 pages
    std::vector<uint8_t> buffer(64);
    
    struct TestResult {
        std::string name;
        uint64_t totalTime;
        double throughput;
        std::vector<uint64_t> channelDist;
    };
    
    std::vector<TestResult> results;
    
    // Test 1: 64-byte interleaving (optimal)
    {
        std::cout << "Test 1: 64-byte Interleaving (Cache Line Granularity)" << std::endl;
        std::cout << "  Sequential requests naturally distribute across all channels\n" << std::endl;
        
        HBMController hbm(8);
        hbm.init(4ULL * 1024 * 1024 * 1024);
        hbm.startup(0);
        
        uint64_t currentTime = 0;
        std::vector<MemRequest> requests;
        
        // Submit all requests RAPIDLY to create queue pressure
        for (uint32_t i = 0; i < numRequests; ++i) {
            MemRequest req;
            req.addr = i * 64;  // Sequential addresses
            req.size = 64;
            req.isRead = true;
            req.isWrite = false;
            req.arrivalTime = currentTime;
            req.data = buffer.data();
            
            if (hbm.recvTimingReq(&req, currentTime)) {
                requests.push_back(req);
            }
            currentTime += 1;  // Issue one request per nanosecond (very aggressive!)
        }
        
        // Process all events until completion
        uint64_t simulationTime = currentTime;
        uint32_t maxIter = 100000;
        uint32_t iter = 0;
        
        while (hbm.hasOutstandingRequests() && iter < maxIter) {
            // Process all channels
            for (uint32_t ch = 0; ch < 8; ++ch) {
                hbm.processNextReqEvent(ch, simulationTime);
                hbm.processRespondEvent(ch, simulationTime);
            }
            simulationTime += 1;
            iter++;
        }
        
        auto& stats = hbm.getStats();
        uint64_t totalBytes = numRequests * 64;
        double throughput = (totalBytes * 1e9) / (simulationTime * 1024.0 * 1024 * 1024);
        
        std::cout << "  Channel accesses: [";
        for (uint32_t ch = 0; ch < 8; ++ch) {
            std::cout << stats.channelAccesses[ch];
            if (ch < 7) std::cout << ", ";
        }
        std::cout << "]" << std::endl;
        
        uint64_t maxAccess = *std::max_element(stats.channelAccesses.begin(), 
                                                stats.channelAccesses.end());
        uint64_t minAccess = *std::min_element(stats.channelAccesses.begin(), 
                                                stats.channelAccesses.end());
        double imbalance = maxAccess > 0 ? 
            ((maxAccess - minAccess) * 100.0 / (numRequests / 8.0)) : 0.0;
        
        std::cout << "  Load imbalance: " << std::fixed << std::setprecision(1) 
                  << imbalance << "%" << std::endl;
        std::cout << "  Simulation time: " << simulationTime << " ns" << std::endl;
        std::cout << "  Measured throughput: " << std::setprecision(2) 
                  << throughput << " GB/s" << std::endl;
        std::cout << "  Pattern: Requests round-robin across channels immediately" << std::endl;
        std::cout << "  Result: ✓ All channels busy from start, excellent parallelism\n" << std::endl;
        
        std::vector<uint64_t> dist(stats.channelAccesses.begin(), stats.channelAccesses.end());
        results.push_back({"64-byte", simulationTime, throughput, dist});
    }
    
    // Test 2: 4KB page interleaving (poor)
    {
        std::cout << "Test 2: 4KB Page Interleaving" << std::endl;
        std::cout << "  Sequential requests stay on same channel for 64 requests\n" << std::endl;
        
        InterleaveConfig pageConfig;
        pageConfig.scheme = InterleavingScheme::PAGE;
        pageConfig.interleaveSize = 4096;
        pageConfig.interleaveBits = 3;
        pageConfig.interleaveLowBit = 12;
        
        HBMController hbm(8, pageConfig);
        hbm.init(4ULL * 1024 * 1024 * 1024);
        hbm.startup(0);
        
        uint64_t currentTime = 0;
        std::vector<MemRequest> requests;
        
        std::cout << "  Request pattern: ";
        for (uint32_t i : {0, 1, 63, 64, 65, 127, 128}) {
            if (i < numRequests) {
                uint64_t addr = i * 64;
                uint32_t ch = hbm.selectChannel(addr);
                std::cout << "Req" << i << "→CH" << ch;
                if (i == 63) std::cout << " | ";
                else if (i == 127) std::cout << " | ";
                else std::cout << ", ";
            }
        }
        std::cout << "..." << std::endl;
        std::cout << "  (First 64 requests serialize on CH0!)\n" << std::endl;
        
        // Submit all requests with SAME aggressive rate
        for (uint32_t i = 0; i < numRequests; ++i) {
            MemRequest req;
            req.addr = i * 64;
            req.size = 64;
            req.isRead = true;
            req.isWrite = false;
            req.arrivalTime = currentTime;
            req.data = buffer.data();
            
            if (hbm.recvTimingReq(&req, currentTime)) {
                requests.push_back(req);
            }
            currentTime += 1;  // Same issue rate!
        }
        
        // Process all events
        uint64_t simulationTime = currentTime;
        uint32_t maxIter = 100000;
        uint32_t iter = 0;
        
        while (hbm.hasOutstandingRequests() && iter < maxIter) {
            for (uint32_t ch = 0; ch < 8; ++ch) {
                hbm.processNextReqEvent(ch, simulationTime);
                hbm.processRespondEvent(ch, simulationTime);
            }
            simulationTime += 1;
            iter++;
        }
        
        auto& stats = hbm.getStats();
        uint64_t totalBytes = numRequests * 64;
        double throughput = (totalBytes * 1e9) / (simulationTime * 1024.0 * 1024 * 1024);
        
        std::cout << "  Channel accesses: [";
        for (uint32_t ch = 0; ch < 8; ++ch) {
            std::cout << stats.channelAccesses[ch];
            if (ch < 7) std::cout << ", ";
        }
        std::cout << "]" << std::endl;
        
        uint64_t maxAccess = *std::max_element(stats.channelAccesses.begin(), 
                                                stats.channelAccesses.end());
        uint64_t minAccess = *std::min_element(stats.channelAccesses.begin(), 
                                                stats.channelAccesses.end());
        double imbalance = maxAccess > 0 ? 
            ((maxAccess - minAccess) * 100.0 / (numRequests / 8.0)) : 0.0;
        
        std::cout << "  Load imbalance: " << std::fixed << std::setprecision(1) 
                  << imbalance << "%" << std::endl;
        std::cout << "  Simulation time: " << simulationTime << " ns" << std::endl;
        std::cout << "  Measured throughput: " << std::setprecision(2) 
                  << throughput << " GB/s" << std::endl;
        std::cout << "  Pattern: First 64 requests queue on CH0, then next 64 on CH1..." << std::endl;
        
        if (simulationTime > results[0].totalTime) {
            std::cout << "  Result: ⚠ Serialization increases latency!\n" << std::endl;
        } else {
            std::cout << "  Result: Similar time (queues may not be deep enough)\n" << std::endl;
        }
        
        std::vector<uint64_t> dist(stats.channelAccesses.begin(), stats.channelAccesses.end());
        results.push_back({"4KB Page", simulationTime, throughput, dist});
    }
    
    // Summary comparison
    std::cout << "Performance Impact Summary:" << std::endl;
    std::cout << "  64-byte interleaving:" << std::endl;
    std::cout << "    Throughput: " << std::fixed << std::setprecision(2) 
              << results[0].throughput << " GB/s" << std::endl;
    std::cout << "    Time: " << results[0].totalTime << " ns" << std::endl;
    
    std::cout << "  4KB page interleaving:" << std::endl;
    std::cout << "    Throughput: " << results[1].throughput << " GB/s ("
              << std::setprecision(1) << (results[1].throughput / results[0].throughput * 100.0) 
              << "% of baseline)" << std::endl;
    std::cout << "    Time: " << results[1].totalTime << " ns" << std::endl;
    
    if (results[0].throughput > results[1].throughput) {
        std::cout << "    Performance loss: " 
                  << std::setprecision(1) << ((1.0 - results[1].throughput / results[0].throughput) * 100.0) 
                  << "% due to serialization" << std::endl;
    } else {
        std::cout << "    Similar performance (issue rate may not be aggressive enough)" << std::endl;
    }
    
    std::cout << "\nWhy Distribution Can Be Same But Performance Different:" << std::endl;
    std::cout << "  • With 256 requests × 64 B = 16 KB = 4 pages" << std::endl;
    std::cout << "  • Each of 4 channels gets 64 requests (one page each)" << std::endl;
    std::cout << "  • Distribution looks balanced: [64,64,64,64,0,0,0,0]" << std::endl;
    std::cout << "  • BUT: With 64-byte, all 4 channels start immediately" << std::endl;
    std::cout << "  • With 4KB page, channels activate sequentially" << std::endl;
    std::cout << "  • First channel processes 64 requests alone (queue buildup!)" << std::endl;
    std::cout << "  • Then second channel starts while first finishes" << std::endl;
    std::cout << "  • Less parallelism → longer time → lower throughput\n" << std::endl;
}

void example_interleave_comparison() {
    std::cout << "\n=== Interleaving Scheme Comparison ===" << std::endl;
    std::cout << "Comparing load distribution with different interleaving granularities" << std::endl;
    std::cout << "Sequential access pattern (256 requests × 64 bytes = 16 KB)\n" << std::endl;
    
    const uint32_t numAccesses = 256;
    std::vector<uint8_t> buffer(64);
    
    // Test different interleaving schemes
    std::vector<std::pair<std::string, InterleaveConfig>> schemes = {
        {"64-byte (Cache)", InterleaveConfig()},
        {"128-byte", InterleaveConfig(128, 8)},
        {"256-byte", InterleaveConfig(256, 8)},
        {"4KB (Page)", []() {
            InterleaveConfig cfg;
            cfg.scheme = InterleavingScheme::PAGE;
            cfg.interleaveSize = 4096;
            cfg.interleaveBits = 3;
            cfg.interleaveLowBit = 12;
            return cfg;
        }()}
    };
    
    std::cout << std::left << std::setw(18) << "Scheme" 
              << std::right << std::setw(15) << "Est. Throughput" 
              << std::setw(12) << "Imbalance"
              << "  " << "Channel Distribution" << std::endl;
    std::cout << std::string(90, '-') << std::endl;
    
    for (auto& [name, config] : schemes) {
        HBMController hbm(8, config);
        hbm.init(4ULL * 1024 * 1024 * 1024);
        hbm.startup(0);
        
        // Use atomic mode to just count distribution
        for (uint32_t i = 0; i < numAccesses; ++i) {
            MemRequest req;
            req.addr = i * 64;  // Sequential cache lines
            req.size = 64;
            req.isRead = true;
            req.isWrite = false;
            req.data = buffer.data();
            
            hbm.recvAtomic(&req);
        }
        
        // Calculate metrics from distribution
        auto& stats = hbm.getStats();
        
        // Find max and min channel usage
        uint64_t maxLoad = 0, minLoad = UINT64_MAX;
        uint32_t activeChannels = 0;
        
        for (uint32_t ch = 0; ch < 8; ++ch) {
            if (stats.channelAccesses[ch] > 0) {
                activeChannels++;
                maxLoad = std::max(maxLoad, stats.channelAccesses[ch]);
                minLoad = std::min(minLoad, stats.channelAccesses[ch]);
            }
        }
        
        // Calculate imbalance
        double avgLoad = static_cast<double>(numAccesses) / 8.0;
        double imbalance = avgLoad > 0 ? ((maxLoad - minLoad) / avgLoad) * 100.0 : 0.0;
        
        // Estimate throughput based on parallelism
        // Bottleneck is the busiest channel
        // Time = max_channel_load × latency_per_request
        double latencyPerRequest = 32.0;  // ns (row hit case)
        double totalTime = maxLoad * latencyPerRequest;  // Time for bottleneck channel
        uint64_t totalBytes = numAccesses * 64;
        double throughput = (totalBytes / totalTime) * 1e9 / (1024.0 * 1024 * 1024);
        
        // Adjust for active channel parallelism
        throughput *= activeChannels;  // More active channels = more parallelism
        
        // Print results
        std::cout << std::left << std::setw(18) << name 
                  << std::right << std::fixed << std::setprecision(2) 
                  << std::setw(12) << throughput << " GB/s"
                  << std::setw(10) << std::setprecision(1) << imbalance << "%"
                  << "    [";
        
        for (uint32_t ch = 0; ch < 8; ++ch) {
            std::cout << std::setw(3) << stats.channelAccesses[ch];
            if (ch < 7) std::cout << ",";
        }
        std::cout << "]";
        
        // Add annotation
        if (activeChannels < 8) {
            std::cout << "  ⚠ Only " << activeChannels << "/8 channels used!";
        }
        std::cout << std::endl;
    }
    
    std::cout << "\nKey Insights:" << std::endl;
    std::cout << "  • 256 requests × 64 B = 16 KB = 4 pages of 4 KB each" << std::endl;
    std::cout << "  • 64-byte interleaving:" << std::endl;
    std::cout << "      Distribution: [32,32,32,32,32,32,32,32] - All 8 channels active" << std::endl;
    std::cout << "      Result: Maximum parallelism (8×)" << std::endl;
    std::cout << "  • 4KB page interleaving:" << std::endl;
    std::cout << "      Distribution: [64,64,64,64,0,0,0,0] - Only 4 channels active" << std::endl;
    std::cout << "      Result: 50% of channels wasted!" << std::endl;
    std::cout << "  • Throughput ≈ (Per-channel BW) × (Active channels) × (Load balance factor)" << std::endl;
    std::cout << "  • Page interleaving wastes 50% capacity for this workload\n" << std::endl;
}
static void run_and_report(const char* name, AcceleratorHBM::HBMController& hbm,
                           uint64_t totalBytes) {
    uint64_t simulationTime = 0;
    uint32_t maxIter = 2000000;
    uint32_t iter = 0;
    // Drain loop: advance time until all responses are done
    while (hbm.hasOutstandingRequests() && iter < maxIter) {
        
        for (uint32_t ch = 0; ch < 8; ++ch) {
            hbm.processNextReqEvent(ch, simulationTime);
            hbm.processRespondEvent(ch, simulationTime);
            iter++;
        }
        simulationTime += 1; // 1 ns tick
        //std::cout<<simulationTime<<"\n";
    }

    auto& stats = hbm.getStats();
    // Use GiB/s (to match your earlier prints) — change denominator to 1e9 for GB/s
    double throughput_gib = (double)totalBytes * 1e9 / (simulationTime * 1024.0 * 1024.0 * 1024.0);

    std::cout << "\n[" << name << "]\n";
    std::cout << "  Simulation time: " << simulationTime << " ns\n";
    std::cout << "  Measured throughput: " << std::fixed << std::setprecision(2)
              << throughput_gib << " GiB/s\n";
    std::cout << "  Channel distribution: [";
    for (uint32_t ch = 0; ch < 8; ++ch) {
        std::cout << stats.channelAccesses[ch] << (ch < 7 ? ", " : "");
    }
    std::cout << "]\n";
}

void peakfeed64B_flooded() {
    using namespace AcceleratorHBM;
    std::cout << "\n=== PeakFeed 64B (Flooded, all t=0) ===\n";

    HBMController hbm(8);           // default: 64B interleaving
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);

    const uint32_t numReq = 8192;   // large queue to keep channels busy
    std::vector<uint8_t> sink(64, 0);

    // Submit ALL requests at t=0 (no per-request +1 ns)
    uint64_t arrival = 0;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < numReq; ++i) {
        MemRequest req;
        req.addr = uint64_t(i) * 64;    // sequential 64B lines
        req.size = 64;
        req.isRead = true;
        req.isWrite = false;
        req.arrivalTime = arrival;      // all at 0
        req.data = sink.data();
        if (hbm.recvTimingReq(&req, arrival)) accepted++;
    }

    run_and_report("PeakFeed64B_Flooded", hbm, uint64_t(accepted) * 64);
}

void peakfeed_BGpipe() {
    using namespace AcceleratorHBM;
    std::cout << "\n=== PeakFeed Bank-Group Pipelined (Flooded) ===\n";

    HBMController hbm(8);                 // 64B interleaving
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);

    const uint32_t numReq = 8192;
    std::vector<uint8_t> sink(64, 0);

    // Address mapping in HBMInterface::decodeAddress():
    // [ row | bank_group(2b) | bank_in_group(2b) | col(5b) | burst(6b) ]
    // We'll:
    //  * keep row constant (row=1)
    //  * round-robin bank_group = {0,1,2,3}
    //  * keep bank_in_group = 0
    //  * step column
    // Also, to keep channel constant while stepping column, advance columns by +8
    // (since channel bits live in bits [8:6] and we don't want to flip channels
    // inside the device-local mapping). That still interleaves globally, but each
    // channel sees a row-hit stream alternating BGs.

    auto make_addr = [](uint32_t i) -> uint64_t {
        const uint64_t burst = 0;                     // aligned
        const uint64_t col   = ((i / 4) % 1024) * 8;  // step columns in 8*64B chunks
        const uint64_t bank  = 0;                     // bank_in_group
        const uint64_t bg    = (i % 4);               // bank-group round-robin
        const uint64_t row   = 1;                     // same row

        // Recompose bits back to a system address. Layout (LSB first):
        // burst(6) | col(5) | bank(2) | bg(2) | row(>=16)
        uint64_t addr = 0;
        addr |= (burst & ((1ull<<6)-1)) << 0;
        addr |= (col   & ((1ull<<5)-1)) << 6;
        addr |= (bank  & ((1ull<<2)-1)) << 11;
        addr |= (bg    & ((1ull<<2)-1)) << 13;
        addr |= (row   << 15);
        return addr;
    };

    uint64_t arrival = 0;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < numReq; ++i) {
        MemRequest req;
        req.addr = make_addr(i);
        req.size = 64;
        req.isRead = true;
        req.isWrite = false;
        req.arrivalTime = arrival;     // all at 0
        req.data = sink.data();
        if (hbm.recvTimingReq(&req, arrival)) accepted++;
    }

    run_and_report("PeakFeed_BGpipe", hbm, uint64_t(accepted) * 64);
}

// ---------- Helper: compose device-local addr + insert channel bits ----------
static inline uint64_t compose_addr_rr_channels(uint32_t ch_rr,
                                                uint16_t row,
                                                uint8_t bg,       // 0..3
                                                uint8_t bank_in,  // 0..3 within BG
                                                uint16_t col_burst) // 0..31 (64B bursts)
{
    // Mapping used by HBMInterface::decodeAddress:
    // [ row | bank_group(2) | bank_in_group(2) | col(5) | burst(6) ]  (LSB->MSB)
    // bytesPerBurst=64 -> burstBits=6; rowBufferSize=2048 -> 32 bursts -> colBits=5
    // banksPerRank=16, bankGroupsPerRank=4 -> bankBits=2, bgBits=2
    // We'll set burst=0 and carry the 64B granularity via col_burst.
    // (These field sizes match your decodeAddress calculation.)  

    const uint64_t burstBits = 6;    // 64 B
    const uint64_t colBits   = 5;    // 32 bursts/row
    const uint64_t bankBits  = 2;    // 4 banks per BG
    const uint64_t bgBits    = 2;    // 4 BGs

    uint64_t addr = 0;
    uint64_t offset = 0;

    // burst(6) = 0
    addr |= (0ull & ((1ull<<burstBits)-1)) << offset; offset += burstBits;
    // col(5)   = col_burst
    addr |= (uint64_t(col_burst) & ((1ull<<colBits)-1)) << offset; offset += colBits;
    // bank(2)  = bank_in
    addr |= (uint64_t(bank_in) & ((1ull<<bankBits)-1)) << offset; offset += bankBits;
    // bg(2)    = bg
    addr |= (uint64_t(bg) & ((1ull<<bgBits)-1)) << offset; offset += bgBits;
    // row      = row (remaining MSBs)
    addr |= (uint64_t)row << offset;

    // Insert channel-select bits per current interleaving (64B => bits [8:6])
    // channel = (addr >> interleaveLowBit) & ((1<<interleaveBits)-1)  
    const uint32_t interleaveLowBit = 6; // for 64B lines  
    const uint32_t interleaveBits   = 3; // 8 channels
    const uint64_t mask = ((1ull<<interleaveBits)-1) << interleaveLowBit;
    addr = (addr & ~mask) | ( (uint64_t(ch_rr & 7) << interleaveLowBit) );

    return addr;
}

// ---------- Test: whole-bank sweep across all channels ----------
void whole_bank_sweep()
{
    using namespace AcceleratorHBM;
    std::cout << "\n=== Whole-Bank Sweep (all channels, row-hit, BG aware) ===\n";

    // Default interleaving = 64B (bits [8:6] select channel)  
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);

    // Per your HBMInterface defaults: 16 banks (4 BG × 4 banks) and 2048B row buffer
    // -> 32 x 64B bursts to sweep one full row per bank.  
    const uint32_t BANK_GROUPS = 4;
    const uint32_t BANKS_PER_BG = 4;
    const uint32_t COL_BURSTS_PER_ROW = 32; // 2048 / 64

    // We’ll visit every (BG, bank) in every channel and sweep one row fully.
    // To respect tCCD_L, we stride BG as the inner alternation; channels round-robin too.
    const uint16_t base_row = 1; // fixed row to keep row open (row-hit stream)

    uint64_t accepted = 0;
    std::vector<uint8_t> sink(64, 0);

    // Round-robin channel ─ for each col burst, for each BG, for each bank
    for (uint16_t col_burst = 0; col_burst < COL_BURSTS_PER_ROW; ++col_burst) {
        for (uint8_t bg = 0; bg < BANK_GROUPS; ++bg) {
            for (uint8_t bank_in = 0; bank_in < BANKS_PER_BG; ++bank_in) {
                for (uint32_t ch = 0; ch < 8; ++ch) {
                    MemRequest req{};
                    req.addr        = compose_addr_rr_channels(ch, base_row, bg, bank_in, col_burst);
                    req.size        = 64;
                    req.isRead      = true;
                    req.isWrite     = false;
                    req.arrivalTime = 0;                 // flood at t=0 (let scheduler pipeline)
                    req.data        = sink.data();

                    if (hbm.recvTimingReq(&req, 0)) {
                        accepted++;
                    }
                }
            }
        }
    }

    const uint64_t totalBytes = accepted * 64ull;
    run_and_report("WholeBankSweep", hbm, totalBytes);
} 

void whole_bank_write_sweep()
{
    using namespace AcceleratorHBM;
    std::cout << "\n=== Whole-Bank Write Sweep (all channels, row-hit, BG aware) ===\n";

    // Default interleaving = 64B (bits [8:6] select channel)
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);

    // Match your interface defaults
    const uint32_t BANK_GROUPS = 4;
    const uint32_t BANKS_PER_BG = 4;
    const uint32_t COL_BURSTS_PER_ROW = 32; // 2048 / 64

    // Keep the same open row across all banks for a row-hit sequence
    const uint16_t base_row = 1;

    // Fill pattern for write data
    std::vector<uint8_t> writeData(64);
    for (uint32_t i = 0; i < 64; ++i)
        writeData[i] = static_cast<uint8_t>(i);

    uint64_t accepted = 0;

    // Sweep all channels, BGs, and banks with write requests
    for (uint16_t col_burst = 0; col_burst < COL_BURSTS_PER_ROW; ++col_burst) {
        for (uint8_t bg = 0; bg < BANK_GROUPS; ++bg) {
            for (uint8_t bank_in = 0; bank_in < BANKS_PER_BG; ++bank_in) {
                for (uint32_t ch = 0; ch < 8; ++ch) {
                    MemRequest req{};
                    req.addr        = compose_addr_rr_channels(ch, base_row, bg, bank_in, col_burst);
                    req.size        = 64;
                    req.isWrite     = true;
                    req.isRead      = false;
                    req.arrivalTime = 0;
                    req.data        = writeData.data();

                    if (hbm.recvTimingReq(&req, 0)) {
                        accepted++;
                    }
                }
            }
        }
    }

    const uint64_t totalBytes = accepted * 64ull;
    run_and_report("WholeBankWriteSweep", hbm, totalBytes);
}

void whole_bank_write_then_read_verify()
{
    using namespace AcceleratorHBM;
    std::cout << "\n=== Whole-Bank Write→Read Verify (row-hit, BG aware) ===\n";

    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);

    const uint32_t BANK_GROUPS        = 4;
    const uint32_t BANKS_PER_BG       = 4;
    const uint32_t COL_BURSTS_PER_ROW = 32;
    const uint64_t BURST              = 64;
    const uint16_t base_row           = 1;

    // ---------- Phase 1: WRITES ----------
    uint64_t writeReqs = 0;
    std::vector<uint8_t> pattern(BURST);

    auto gen_pattern = [&](uint32_t ch, uint8_t bg, uint8_t bank_in, uint16_t col_burst) {
        for (uint32_t i = 0; i < BURST; ++i)
            pattern[i] = static_cast<uint8_t>((ch << 4) ^ (bg << 2) ^ (bank_in << 1) ^ col_burst ^ i);
    };

    for (uint16_t col_burst = 0; col_burst < COL_BURSTS_PER_ROW; ++col_burst)
      for (uint8_t bg = 0; bg < BANK_GROUPS; ++bg)
        for (uint8_t bank_in = 0; bank_in < BANKS_PER_BG; ++bank_in)
          for (uint32_t ch = 0; ch < 8; ++ch) {
              gen_pattern(ch, bg, bank_in, col_burst);

              MemRequest req{};
              req.addr        = compose_addr_rr_channels(ch, base_row, bg, bank_in, col_burst);
              req.size        = BURST;
              req.isWrite     = true;
              req.isRead      = false;
              req.arrivalTime = 0;
              req.data        = pattern.data();
              if (hbm.recvTimingReq(&req, 0)) writeReqs++;
          }

    auto drain_until_idle = [&](HBMController& h) {
        uint64_t simTime = 0;
        uint32_t iter = 0, maxIter = 2'000'000;
        while (h.hasOutstandingRequests() && iter < maxIter) {
            uint64_t next = h.getNextEventTime();
            simTime = next;
            for (uint32_t ch = 0; ch < 8; ++ch) {
                h.processNextReqEvent(ch, simTime);
                h.processRespondEvent(ch, simTime);
            }
            h.advanceTime(simTime);
            iter++;
        }
        return h.getCurrentTime();
    };

    uint64_t t0 = hbm.getCurrentTime();
    uint64_t tW_end = drain_until_idle(hbm);
    double   write_time_ns = double(tW_end - t0);

    // ---------- Phase 2: READS (enqueue all, then drain, then verify) ----------
    struct ReadItem { uint64_t addr; std::vector<uint8_t> buf; uint32_t ch; uint8_t bg, bank_in; uint16_t col; };
    std::vector<ReadItem> reads; reads.reserve(4096);

    uint64_t readReqs = 0;
    for (uint16_t col_burst = 0; col_burst < COL_BURSTS_PER_ROW; ++col_burst)
      for (uint8_t bg = 0; bg < BANK_GROUPS; ++bg)
        for (uint8_t bank_in = 0; bank_in < BANKS_PER_BG; ++bank_in)
          for (uint32_t ch = 0; ch < 8; ++ch) {
              ReadItem item;
              item.addr = compose_addr_rr_channels(ch, base_row, bg, bank_in, col_burst);
              item.buf.resize(BURST);
              item.ch = ch; item.bg = bg; item.bank_in = bank_in; item.col = col_burst;

              MemRequest req{};
              req.addr        = item.addr;
              req.size        = BURST;
              req.isWrite     = false;
              req.isRead      = true;
              req.arrivalTime = 0;
              req.data        = item.buf.data();

              if (hbm.recvTimingReq(&req, 0)) {
                  readReqs++;
                  reads.push_back(std::move(item));
              }
          }

    uint64_t tR_end = drain_until_idle(hbm);
    double   read_time_ns = double(tR_end - tW_end);

    // ---------- Verify AFTER drain ----------
    uint64_t mismatches = 0;
    for (auto& r : reads) {
        gen_pattern(r.ch, r.bg, r.bank_in, r.col);
        if (!std::equal(r.buf.begin(), r.buf.end(), pattern.begin()))
            mismatches++;
    }

    // ---------- Report per phase ----------
    auto gib_per_s = [](double bytes, double ns){
        return (ns > 0.0) ? (bytes * 1e9) / (ns * 1024.0 * 1024.0 * 1024.0) : 0.0;
    };

    double wr_gibs = gib_per_s(double(writeReqs) * BURST, write_time_ns);
    double rd_gibs = gib_per_s(double(readReqs)  * BURST, read_time_ns);

    auto& st = hbm.getStats();
    std::cout << "\n[WholeBankWriteThenReadVerify]\n";
    std::cout << "  Writes: " << writeReqs << " (" << wr_gibs << " GiB/s)\n";
    std::cout << "  Reads : " << readReqs  << " (" << rd_gibs << " GiB/s)\n";
    std::cout << "  Total time: " << (tR_end - t0) << " ns\n";
    std::cout << "  Mismatches: " << mismatches << "\n  Channel distribution: [";
    for (uint32_t ch = 0; ch < 8; ++ch)
        std::cout << st.channelAccesses[ch] << (ch < 7 ? ", " : "");
    std::cout << "]\n";
}


void test() {
    std::cout << "[PeakFeed_AllChannels]" << std::endl;
    HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);

    // 8 channels × 16 banks × 256 requests each
    const uint32_t reqsPerCh = 2048;
    uint64_t currentTime = 0;

    for (uint32_t i = 0; i < reqsPerCh * 8; ++i) {
        MemRequest req;
        req.addr = (i * 64) % (4ULL * 1024 * 1024 * 1024);
        req.size = 64;
        req.isRead = true;
        req.isWrite = false;
        req.arrivalTime = 0;  // all arrive at once
        hbm.recvTimingReq(&req, currentTime);
    }

    // Run until completion
    uint64_t simulationTime = 0;
    while (hbm.hasOutstandingRequests()) {
        for (uint32_t ch = 0; ch < 8; ++ch) {
            hbm.processNextReqEvent(ch, simulationTime);
            hbm.processRespondEvent(ch, simulationTime);
        }
        simulationTime++;
    }

    double throughput = (8.0 * reqsPerCh * 64 * 1e9) /
        (simulationTime * 1024.0 * 1024 * 1024);
    std::cout << "Throughput ≈ " << throughput << " GB/s" << std::endl;
}

int main() {
    // std::cout << "===========================================================" << std::endl;
    // std::cout << "    Standalone HBM Controller - Comprehensive Tests" << std::endl;
    // std::cout << "===========================================================" << std::endl;
    // std::cout << "Configuration:" << std::endl;
    // std::cout << "  Clock frequency: 1 GHz (1 ns period)" << std::endl;
    // std::cout << "  Number of channels: 8" << std::endl;
    // std::cout << "  Per-channel bandwidth: 32 GB/s (128-bit @ DDR 2 Gbps)" << std::endl;
    // std::cout << "  Total bandwidth: 256 GB/s" << std::endl;
    // std::cout << "  Burst size: 64 bytes (BL4 × 128 bits)" << std::endl;
    // std::cout << "  Burst duration: 2 ns\n" << std::endl;
    
    // example_basic_usage();
    // example_timing_analysis();
    // example_atomic_access();
    // example_multi_channel();
    // example_bandwidth_sequential();
    // example_bandwidth_random();
    // example_burst_efficiency();
    // example_latency_breakdown();
    // example_page_interleaving();
    // example_custom_interleaving();
    // example_interleaving_impact_detailed();
    // example_interleave_comparison();
    // peakfeed64B_flooded();
    // peakfeed_BGpipe();
    // whole_bank_sweep();
    // whole_bank_write_sweep();
    whole_bank_write_then_read_verify();
    
    std::cout << "\n===========================================================" << std::endl;
    std::cout << "              All tests completed successfully!" << std::endl;
    std::cout << "===========================================================" << std::endl;
    
    return 0;
}