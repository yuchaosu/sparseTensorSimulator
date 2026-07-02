/*
 * Standalone HBM Controller for Custom Accelerator
 * Based on gem5 HBM logic but independent implementation
 * 
 * Configuration: 256 GB/s, 8 channels, 1GHz clock
 */

#ifndef ACCELERATOR_HBM_H
#define ACCELERATOR_HBM_H

#include <cstdint>
#include <queue>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <cassert>
#include <cstdio>
#include <string>

namespace AcceleratorHBM {

// Interleaving configuration
enum class InterleavingScheme {
    CACHE_LINE,      // 64-byte interleaving (default)
    PAGE,            // 4KB page interleaving
    CUSTOM           // User-defined
};

struct InterleaveConfig {
    InterleavingScheme scheme;
    uint32_t interleaveBits;      // Number of bits for channel selection
    uint32_t interleaveSize;      // Size in bytes (e.g., 64, 128, 4096)
    uint32_t interleaveLowBit;    // LSB position for interleave bits
    
    InterleaveConfig() {
        scheme = InterleavingScheme::CACHE_LINE;
        interleaveBits = 3;       // log2(8 channels) = 3
        interleaveSize = 64;      // 64-byte cache line
        interleaveLowBit = 6;     // Start at bit 6 (2^6 = 64)
    }
    
    // Constructor for custom interleaving
    InterleaveConfig(uint32_t size, uint32_t numChannels) {
        scheme = InterleavingScheme::CUSTOM;
        interleaveSize = size;
        interleaveBits = 0;
        uint32_t temp = numChannels;
        while (temp > 1) {
            interleaveBits++;
            temp >>= 1;
        }
        interleaveLowBit = 0;
        temp = size;
        while (temp > 1) {
            interleaveLowBit++;
            temp >>= 1;
        }
    }
};

// Timing parameters (in nanoseconds)
struct HBMTimingParams {
    // Clock period
    double tCK;          // 1ns for 1GHz
    
    // Row timing
    double tRP;          // Row precharge time: 14ns
    double tRCD;         // RAS to CAS delay: 12ns
    double tRCD_WR;      // Write RAS to CAS delay: 6ns
    double tRAS;         // Row active time: 28ns
    
    // Column timing
    double tCL;          // CAS latency: 18ns
    double tCWL;         // CAS write latency: 7ns
    double tCCD_L;       // Column to column delay: 3ns
    double tCCD_S;       // Column to column delay (same bank group): 2ns
    double tBURST;       // Burst duration: 2ns
    
    // Refresh timing
    double tRFC;         // Refresh cycle time: 220ns
    double tREFI;        // Refresh interval: 3900ns
    
    // Read/Write timing
    double tWR;          // Write recovery time: 14ns
    double tRTP;         // Read to precharge: 5ns
    double tWTR;         // Write to read delay: 4ns
    double tWTR_L;       // Write to read delay (same bank group): 9ns
    double tRTW;         // Read to write delay: 18ns
    
    // Activate timing
    double tRRD;         // Activate to activate delay: 4ns
    double tRRD_L;       // Activate to activate (same bank group): 6ns
    double tXAW;         // Four activate window: 16ns
    
    // Power timing
    double tXP;          // Power down exit: 8ns
    double tXS;          // Self refresh exit: 216ns
    
    // Helper to calculate tRC (RAS cycle time)
    double tRC() const { return tRAS + tRP; }
    
    HBMTimingParams() {
        // Default values for 1GHz HBM2
        tCK = 1.0;
        tRP = 14.0;
        tRCD = 12.0;
        tRCD_WR = 6.0;
        tRAS = 28.0;
        tCL = 18.0;
        tCWL = 7.0;
        tCCD_L = 3.0;
        tCCD_S = 2.0;
        tBURST = 2.0;
        tRFC = 220.0;
        tREFI = 3900.0;
        tWR = 14.0;
        tRTP = 5.0;
        tWTR = 4.0;
        tWTR_L = 9.0;
        tRTW = 18.0;
        tRRD = 4.0;
        tRRD_L = 6.0;
        tXAW = 16.0;
        tXP = 8.0;
        tXS = 216.0;
    }
};

// Memory request packet
struct MemRequest {
    uint64_t addr;
    uint32_t size;
    bool isWrite;
    bool isRead;
    uint64_t arrivalTime;
    uint64_t readyTime;
    uint8_t qosValue;
    uint32_t requestorId;
    void* data;
    
    MemRequest() : addr(0), size(0), isWrite(false), isRead(false),
                   arrivalTime(0), readyTime(0), qosValue(0),
                   requestorId(0), data(nullptr) {}
};

// Memory packet for internal processing
struct MemPacket {
    uint64_t addr;
    uint32_t size;
    bool isRead;
    uint8_t rank;
    uint8_t bank;
    uint16_t row;
    uint16_t col;
    uint8_t bankGroup;
    uint8_t pseudoChannel;
    uint64_t entryTime;
    uint64_t readyTime;
    uint8_t qosValue;
    uint32_t requestorId;
    MemRequest* pkt;
    
    MemPacket() : addr(0), size(0), isRead(false), rank(0), bank(0),
                  row(0), col(0), bankGroup(0), pseudoChannel(0),
                  entryTime(0), readyTime(UINT64_MAX), qosValue(0),
                  requestorId(0), pkt(nullptr) {}
};

// Bank state
struct BankState {
    enum State {
        IDLE,
        ACTIVE,
        PRECHARGING,
        REFRESHING
    };
    
    State state;
    uint16_t openRow;
    uint64_t actAllowedAt;
    uint64_t preAllowedAt;
    uint64_t colAllowedAt;
    
    BankState() : state(IDLE), openRow(0), actAllowedAt(0),
                  preAllowedAt(0), colAllowedAt(0) {}
};

// HBM Interface
class HBMInterface {
public:
    HBMInterface(uint32_t channelId, const HBMTimingParams& timing);
    ~HBMInterface();
    
    // Configuration
    void setAddrRange(uint64_t start, uint64_t size);
    void setController(void* ctrl);
    
    // Address mapping
    void decodeAddress(uint64_t addr, uint8_t& rank, uint8_t& bank,
                      uint16_t& row, uint16_t& col, uint8_t& bankGroup);
    uint64_t burstAlign(uint64_t addr);
    
    // Timing checks
    bool canIssueActivate(uint8_t bank, uint64_t currentTime);
    bool canIssuePrecharge(uint8_t bank, uint64_t currentTime);
    bool canIssueColumn(uint8_t bank, uint64_t currentTime);
    bool isRowHit(uint8_t bank, uint16_t row);
    
    // Command scheduling
    uint64_t scheduleActivate(uint8_t bank, uint16_t row, uint64_t currentTime);
    uint64_t schedulePrecharge(uint8_t bank, uint64_t currentTime);
    uint64_t scheduleColumn(uint8_t bank, bool isRead, uint64_t currentTime);
    
    // Access methods
    uint64_t doAccess(MemPacket* pkt, uint64_t currentTime);
    
    // Queue management
    void addToReadQueue(MemPacket* pkt);
    void addToWriteQueue(MemPacket* pkt);
    MemPacket* chooseNextRead(uint64_t currentTime);
    MemPacket* chooseNextWrite(uint64_t currentTime);
    
    // Getters
    uint32_t getReadQueueSize() const { return readQueue.size(); }
    uint32_t getWriteQueueSize() const { return writeQueue.size(); }
    uint32_t getBytesPerBurst() const { return bytesPerBurst; }
    uint64_t getNextBurstAt() const { return nextBurstAt; }
    bool getAddrRangeContains(uint64_t addr) const {
        return (addr >= rangeStart && addr < rangeEnd);
    }
    
public:
    // Configuration parameters
    uint32_t channelId;
    uint32_t deviceBusWidth;      // 128 bits
    uint32_t burstLength;          // 4
    uint32_t bytesPerBurst;        // 64 bytes (128 bits * 4 / 8)
    uint32_t banksPerRank;         // 16
    uint32_t bankGroupsPerRank;    // 4
    uint32_t ranksPerChannel;      // 1
    uint32_t rowBufferSize;        // 2048 bytes
    uint64_t deviceSize;           // 512 MiB per channel
    uint64_t nextBusFreeAt = 0;           // shared channel data-bus token 
    uint64_t lastIssueAtBG[4] = {0,0,0,0}; // per bank-group last CAS issue
    
    // Address range
    uint64_t rangeStart;
    uint64_t rangeEnd;
    
    // Timing parameters
    HBMTimingParams timing;
    
    // Bank states
    std::vector<BankState> banks;
    
    // Queues
    std::vector<MemPacket*> readQueue;
    std::vector<MemPacket*> writeQueue;
    
    // Timing tracking
    uint64_t nextBurstAt;
    uint64_t nextReqTime;
    
    // Statistics
    uint64_t readsThisTime;
    uint64_t writesThisTime;
    
    // Bus state
    enum BusState {
        READ,
        WRITE
    };
    BusState busState;
    BusState busStateNext;
    
private:
    void* controller;  // Pointer back to controller
    
    // Address mapping helpers
    uint32_t bitsNeeded(uint64_t value);
    uint64_t extractBits(uint64_t addr, uint32_t lsb, uint32_t width);
};

// HBM Controller
class HBMController {
public:
    HBMController(uint32_t numChannels);
    HBMController(uint32_t numChannels, const InterleaveConfig& config);
    ~HBMController();
    
    // Initialize controller
    void init(uint64_t totalMemorySize);
    void startup(uint64_t currentTime);
    
    // Configure interleaving
    void setInterleaveConfig(const InterleaveConfig& config);
    InterleaveConfig getInterleaveConfig() const { return interleaveConfig; }
    
    // Request handling
    bool recvTimingReq(MemRequest* pkt, uint64_t currentTime);
    uint64_t recvAtomic(MemRequest* pkt);
    
    // Processing
    void processNextReqEvent(uint32_t channelId, uint64_t currentTime);
    void processRespondEvent(uint32_t channelId, uint64_t currentTime);
    
    // Channel selection (with interleaving)
    uint32_t selectChannel(uint64_t addr) const;
    uint64_t getChannelAddr(uint64_t systemAddr, uint32_t channel) const;
    
    // Queue checks
    bool readQueueFull(uint32_t channelId, uint32_t neededEntries);
    bool writeQueueFull(uint32_t channelId, uint32_t neededEntries);
    
    // Scheduling
    void scheduleNextRequest(uint32_t channelId, uint64_t time);
    void scheduleResponse(uint32_t channelId, uint64_t time);
    
    // Event-driven simulation helpers
    uint64_t getNextEventTime() const;
    bool hasOutstandingRequests() const;
    uint64_t getCurrentTime() const { return currentTime; }
    void advanceTime(uint64_t time) { currentTime = time; }
    
    // Helper to print interleave configuration
    void printInterleaveConfig() const;
    
    // Statistics
    struct Stats {
        uint64_t readReqs;
        uint64_t writeReqs;
        uint64_t readBursts;
        uint64_t writeBursts;
        uint64_t bytesRead;
        uint64_t bytesWritten;
        uint64_t totalReadLatency;
        uint64_t totalWriteLatency;
        std::vector<uint64_t> channelAccesses;  // Per-channel access counts
        
        Stats() : readReqs(0), writeReqs(0), readBursts(0), writeBursts(0),
                  bytesRead(0), bytesWritten(0), totalReadLatency(0),
                  totalWriteLatency(0) {}
    };
    
    Stats& getStats() { return stats; }
    void printStats();

    // Tracing API (optional runtime tracing to CSV)
    // Each trace line format (CSV):
    // timestamp_ns,channel,evt,addr,size,requestorId
    // evt = ENQ/COMP, addr in hex
    void enableTrace(const std::string& path);
    void disableTrace();
    // Aggregate tracing: record totals (compact) instead of per-operation lines
    void enableTraceAggregate(const std::string& path);
    void disableTraceAggregate();
    bool isTraceEnabled() const { return traceEnabled; }
    
private:
    // Configuration
    uint32_t numChannels;
    uint32_t readBufferSize;
    uint32_t writeBufferSize;
    uint32_t writeHighThreshold;
    uint32_t writeLowThreshold;
    uint64_t commandWindow;
    
    // Interleaving configuration
    InterleaveConfig interleaveConfig;
    
    // Channels
    std::vector<HBMInterface*> channels;
    
    // Response queues (one per channel)
    std::vector<std::queue<MemPacket*>> respQueues;
    
    // Command scheduling
    std::set<uint64_t> burstTicks;
    std::set<uint64_t> rowBurstTicks;
    std::set<uint64_t> colBurstTicks;
    
    // Retry flags
    std::vector<bool> retryRdReq;
    std::vector<bool> retryWrReq;
    
    // Statistics
    Stats stats;
    
    // Helpers
    uint64_t getBurstWindow(uint64_t cmdTick);
    uint64_t verifySingleCmd(uint64_t cmdTick, uint64_t maxCmdsPerBurst, bool rowCmd);
    uint64_t verifyMultiCmd(uint64_t cmdTick, uint64_t maxCmdsPerBurst, 
                           uint64_t maxMultiCmdSplit);
    void pruneBurstTick(uint64_t currentTime);
    
    // Memory allocation tracking
    std::vector<uint8_t> memory;
    uint64_t totalMemorySize;
    uint64_t currentTime;  // Track simulation time

    // Tracing
    bool traceEnabled = false;
    FILE* traceFp = nullptr;
    // Aggregate tracing state
    bool traceAggregateEnabled = false;
    FILE* traceAggregateFp = nullptr;
    uint64_t agg_total_bytes = 0;
    uint64_t agg_enq_count = 0;
    uint64_t agg_comp_count = 0;
    uint64_t agg_first_ts = 0;
    uint64_t agg_last_ts = 0;
};

} // namespace AcceleratorHBM

#endif // ACCELERATOR_HBM_H