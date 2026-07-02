#include "AcceleratorHBM.h"
#include <iostream>
#include <iomanip>

using namespace AcceleratorHBM;

int main() {
    std::cout << "=== Channel Selection Debug Test ===\n" << std::endl;
    
    // Test 1: 64-byte interleaving
    {
        std::cout << "Test 1: 64-byte Cache Line Interleaving" << std::endl;
        std::cout << "Expected: First 8 requests should go to channels 0-7\n" << std::endl;
        
        HBMController hbm(8);
        hbm.init(4ULL * 1024 * 1024 * 1024);
        
        std::cout << "First 16 sequential addresses:" << std::endl;
        std::cout << "Addr       | Hex    | Channel" << std::endl;
        std::cout << std::string(35, '-') << std::endl;
        
        for (uint32_t i = 0; i < 16; ++i) {
            uint64_t addr = i * 64;
            uint32_t ch = hbm.selectChannel(addr);
            std::cout << std::setw(6) << addr << " (0x" 
                      << std::hex << std::setw(4) << std::setfill('0') << addr 
                      << std::dec << ") | CH" << ch << std::endl;
        }
        std::cout << std::endl;
    }
    
    // Test 2: 4KB page interleaving
    {
        std::cout << "Test 2: 4KB Page Interleaving" << std::endl;
        std::cout << "Expected: First 64 requests (0-4032 bytes) should ALL go to CH0\n" << std::endl;
        
        InterleaveConfig pageConfig;
        pageConfig.scheme = InterleavingScheme::PAGE;
        pageConfig.interleaveSize = 4096;
        pageConfig.interleaveBits = 3;
        pageConfig.interleaveLowBit = 12;  // Bits [14:12] select channel
        
        HBMController hbm(8, pageConfig);
        hbm.init(4ULL * 1024 * 1024 * 1024);
        
        hbm.printInterleaveConfig();
        
        std::cout << "Checking critical addresses:" << std::endl;
        std::cout << "Addr       | Hex     | Bits[14:12] | Channel | Expected" << std::endl;
        std::cout << std::string(65, '-') << std::endl;
        
        // First page (0-4095)
        std::cout << "First page (should all be CH0):" << std::endl;
        for (uint32_t i : {0, 1, 2, 63}) {  // Key addresses
            uint64_t addr = i * 64;
            uint32_t ch = hbm.selectChannel(addr);
            uint32_t bits = (addr >> 12) & 0x7;
            std::cout << std::setw(6) << addr << " (0x" 
                      << std::hex << std::setw(5) << std::setfill('0') << addr 
                      << std::dec << ") | " << bits << "           | CH" << ch 
                      << "     | CH0" << std::endl;
        }
        
        // Page boundaries
        std::cout << "\nPage boundaries:" << std::endl;
        for (uint32_t page = 0; page < 8; ++page) {
            uint64_t addr = page * 4096;
            uint32_t ch = hbm.selectChannel(addr);
            uint32_t bits = (addr >> 12) & 0x7;
            std::cout << "Page " << page << " (0x" 
                      << std::hex << std::setw(5) << std::setfill('0') << addr 
                      << std::dec << ") | " << bits << "           | CH" << ch 
                      << "     | CH" << page << std::endl;
        }
        
        // Count distribution for first 512 requests
        std::cout << "\nDistribution for first 512 sequential requests:" << std::endl;
        std::vector<uint32_t> counts(8, 0);
        for (uint32_t i = 0; i < 512; ++i) {
            uint64_t addr = i * 64;
            uint32_t ch = hbm.selectChannel(addr);
            counts[ch]++;
        }
        
        std::cout << "Channel distribution: [";
        for (uint32_t ch = 0; ch < 8; ++ch) {
            std::cout << counts[ch];
            if (ch < 7) std::cout << ", ";
        }
        std::cout << "]" << std::endl;
        
        std::cout << "\nExpected: [64, 64, 64, 64, 64, 64, 64, 64]" << std::endl;
        std::cout << "Why? 512 requests × 64 bytes = 32768 bytes = 8 pages" << std::endl;
        std::cout << "Each page (4KB) gets 64 requests" << std::endl;
    }
    
    // Test 3: Show what happens with page interleaving timing
    {
        std::cout << "\n=== Test 3: Sequential Request Pattern ===" << std::endl;
        
        InterleaveConfig pageConfig;
        pageConfig.scheme = InterleavingScheme::PAGE;
        pageConfig.interleaveSize = 4096;
        pageConfig.interleaveBits = 3;
        pageConfig.interleaveLowBit = 12;
        
        HBMController hbm(8, pageConfig);
        hbm.init(4ULL * 1024 * 1024 * 1024);
        
        std::cout << "First 20 requests show the pattern:" << std::endl;
        std::cout << "Req# | Addr    | Channel | Pattern" << std::endl;
        std::cout << std::string(50, '-') << std::endl;
        
        for (uint32_t i = 0; i < 20; ++i) {
            uint64_t addr = i * 64;
            uint32_t ch = hbm.selectChannel(addr);
            std::cout << std::setw(4) << i << " | 0x" 
                      << std::hex << std::setw(5) << std::setfill('0') << addr 
                      << std::dec << " | CH" << ch;
            
            if (i == 0) std::cout << " | ← First 64 go to CH0";
            if (i == 63) std::cout << " | ← Last of first batch";
            if (i == 64) std::cout << " | ← Next 64 go to CH1";
            
            std::cout << std::endl;
        }
        
        std::cout << "\nThis is why page interleaving is poor for sequential access!" << std::endl;
        std::cout << "Requests 0-63 serialize on CH0 (blocked waiting)" << std::endl;
        std::cout << "Requests 64-127 serialize on CH1 (while CH0 finishes)" << std::endl;
        std::cout << "Limited parallelism!" << std::endl;
    }
    
    return 0;
}