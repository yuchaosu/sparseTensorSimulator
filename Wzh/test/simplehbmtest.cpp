/*
 * Minimal HBM Write Test - No External Dependencies
 * 
 * This test issues a few simple write requests to diagnose the stall issue.
 * Compile: g++ -std=c++17 -o minimal_test minimal_hbm_test.cpp AcceleratorHBM_debug.cpp
 */

#include "../include/AcceleratorHBM.h"
#include <iostream>
#include <cstring>
#include <iomanip>

int main() {
    std::cout << "=== Minimal HBM Write Test ===" << std::endl;
    
    // Initialize HBM with 8 channels, 4 GiB total
    AcceleratorHBM::HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);
    hbm.startup(0);
    
    std::cout << "\n=== Issuing 10 Write Requests ===" << std::endl;
    
    // Create some test data
    uint8_t data[64];
    for (int i = 0; i < 64; i++) {
        data[i] = i;
    }

    // Issue 1000 writes to different addresses
    for (int i = 0; i < 1000; i++) {
        AcceleratorHBM::MemRequest req;
        req.addr = i * 0x1000;  // 4KB apart
        req.size = 64;
        req.isRead = false;
        req.isWrite = true;
        req.arrivalTime = 0;
        req.data = data;
        
        bool success = hbm.recvTimingReq(&req, 0);
        std::cout << "Write #" << i << " to addr 0x" << std::hex << req.addr << std::dec
                  << ": " << (success ? "SUCCESS" : "FAILED") << std::endl;
    }
    
    std::cout << "\n=== Queue Status ===" << std::endl;
    std::cout << "Total write queue size: " << hbm.writeQueueSize() << std::endl;
    std::cout << "Has outstanding requests: " << hbm.hasOutstandingRequests() << std::endl;
    
    // Try to drain
    std::cout << "\n=== Draining (max 10000 cycles) ===" << std::endl;
    uint64_t sim = 1;
    int iterations = 0;
    int maxIter = 10000;
    
    while (hbm.hasOutstandingRequests() && iterations < maxIter) {
        for (uint32_t ch = 0; ch < 8; ch++) {
            hbm.processNextReqEvent(ch, sim);
            hbm.processRespondEvent(ch, sim);
        }
        sim++;
        iterations++;
        
        if (iterations % 1000 == 0) {
            auto stats = hbm.getStats();
            std::cout << "Iter " << iterations << ": time=" << sim 
                      << " writeReqs=" << stats.writeReqs 
                      << " writeBursts=" << stats.writeBursts
                      << " queueSize=" << hbm.writeQueueSize() << std::endl;
        }
    }
    
    std::cout << "\n=== Final Statistics ===" << std::endl;
    auto stats = hbm.getStats();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Simulation time: " << sim << " ns" << std::endl;
    std::cout << "Write requests completed: " << stats.writeReqs << std::endl;
    std::cout << "Write bursts issued: " << stats.writeBursts << std::endl;
    std::cout << "Bytes written: " << stats.bytesWritten << std::endl;
    std::cout << "Remaining in queue: " << hbm.writeQueueSize() << std::endl;
    
    if (iterations >= maxIter) {
        std::cout << "\n*** WARNING: Hit max iterations! System appears stuck. ***" << std::endl;
        std::cout << "Check the debug output above to see where processing stopped." << std::endl;
    } else {
        std::cout << "\n*** SUCCESS: All writes completed! ***" << std::endl;
    }
    
    return 0;
}