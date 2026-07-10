#pragma once

#include <cstdint>
#include <vector>
#include <queue>
#include <set>

namespace AcceleratorHBM {

// Timing parameters for HBM
struct HBMTimingParams {
    uint32_t tCL = 14;      // CAS latency
    uint32_t tRCD = 14;     // RAS to CAS delay
    uint32_t tRP = 14;      // Row precharge time
    uint32_t tRAS = 33;     // Row active time
    uint32_t tWR = 15;      // Write recovery time
    uint32_t tRTP = 8;      // Read to precharge
    uint32_t tCCD_L = 6;    // CAS to CAS (same bank group)
    uint32_t tCCD_S = 4;    // CAS to CAS (different bank group)
    uint32_t tBURST = 4;    // Burst duration
    uint32_t tCWL = 12;     // CAS write latency
    
    uint32_t tRC() const { return tRAS + tRP; }  // Row cycle time
};

// Bank states
enum class BankState {
    IDLE,
    ACTIVE
};

// Bank information
struct BankInfo {
    BankState state = BankState::IDLE;
    uint16_t openRow = 0;
    uint64_t actAllowedAt = 0;
    uint64_t preAllowedAt = 0;
    uint64_t colAllowedAt = 0;
};

// Memory request from user
struct MemRequest {
    uint64_t addr;
    uint32_t size;
    bool isRead = false;
    bool isWrite = false;
    uint64_t arrivalTime;
    uint8_t* data = nullptr;
    uint32_t qosValue = 0;
    uint32_t requestorId = 0;
};

// Internal memory packet
struct MemPacket {
    uint64_t addr;
    uint32_t size;
    bool isRead = false;
    bool isWrite = false;
    uint64_t entryTime;
    uint64_t readyTime;
    uint32_t qosValue;
    uint32_t requestorId;
    MemRequest* pkt = nullptr;
    uint8_t* data = nullptr;
};

// Forward declaration
class HBMController;

// HBM Interface (per channel)
class HBMInterface {
public:
    enum BusState { READ, WRITE };
    
    uint32_t channelId;
    HBMTimingParams timing;
    void* controller;
    
    // Configuration
    uint32_t deviceBusWidth;
    uint32_t burstLength;
    uint32_t bytesPerBurst;
    uint32_t banksPerRank;
    uint32_t bankGroupsPerRank;
    uint32_t ranksPerChannel;
    uint32_t rowBufferSize;
    uint64_t deviceSize;
    
    // State
    std::vector<BankInfo> banks;
    std::vector<MemPacket*> readQueue;
    std::vector<MemPacket*> writeQueue;
    
    uint64_t nextBurstAt;
    uint64_t nextReqTime;
    uint32_t readsThisTime;
    uint32_t writesThisTime;
    
    BusState busState;
    BusState busStateNext;
    
    uint64_t rangeStart;
    uint64_t rangeEnd;
    
    uint64_t nextBusFreeAt;
    uint64_t lastIssueAtBG[4];
    
    HBMInterface(uint32_t channelId, const HBMTimingParams& timing);
    ~HBMInterface();
    
    void setAddrRange(uint64_t start, uint64_t size);
    void setController(void* ctrl);
    
    uint32_t getBytesPerBurst() const { return bytesPerBurst; }
    uint64_t burstAlign(uint64_t addr);
    
    void decodeAddress(uint64_t addr, uint8_t& rank, uint8_t& bank,
                      uint16_t& row, uint16_t& col, uint8_t& bankGroup);
    
    bool canIssueActivate(uint8_t bank, uint64_t currentTime);
    bool canIssuePrecharge(uint8_t bank, uint64_t currentTime);
    bool canIssueColumn(uint8_t bank, uint64_t currentTime);
    bool isRowHit(uint8_t bank, uint16_t row);
    
    uint64_t scheduleActivate(uint8_t bank, uint16_t row, uint64_t currentTime);
    uint64_t schedulePrecharge(uint8_t bank, uint64_t currentTime);
    uint64_t scheduleColumn(uint8_t bank, bool isRead, uint64_t currentTime);
    
    uint64_t doAccess(MemPacket* pkt, uint64_t currentTime);
    
    void addToReadQueue(MemPacket* pkt);
    void addToWriteQueue(MemPacket* pkt);
    
    MemPacket* chooseNextRead(uint64_t currentTime);
    MemPacket* chooseNextWrite(uint64_t currentTime);
    
private:
    uint32_t bitsNeeded(uint64_t value);
    uint64_t extractBits(uint64_t addr, uint32_t lsb, uint32_t width);
};

// Interleaving scheme
enum class InterleavingScheme {
    CACHE_LINE,  // 64-byte cache line interleaving
    PAGE,        // 4 KB page interleaving
    CUSTOM       // Custom interleaving size
};

// Interleaving configuration
struct InterleaveConfig {
    InterleavingScheme scheme = InterleavingScheme::CACHE_LINE;
    uint32_t interleaveSize = 64;     // Size of interleave block in bytes
    uint32_t interleaveBits = 3;      // 8 channels (2^3)
    uint32_t interleaveLowBit = 6;    // Byte 64 (cache line size)
};

// Statistics
struct HBMStats {
    uint64_t readReqs = 0;
    uint64_t writeReqs = 0;
    uint64_t readBursts = 0;
    uint64_t writeBursts = 0;
    uint64_t bytesRead = 0;
    uint64_t bytesWritten = 0;
    uint64_t totalReadLatency = 0;
    uint64_t totalWriteLatency = 0;
    std::vector<uint64_t> channelAccesses;
};

// HBM Controller
class HBMController {
public:
    HBMController(uint32_t numChannels);
    HBMController(uint32_t numChannels, const InterleaveConfig& config);
    ~HBMController();
    
    void init(uint64_t totalSize);
    void startup(uint64_t currentTime);
    
    void setInterleaveConfig(const InterleaveConfig& config);
    void printInterleaveConfig() const;
    
    bool recvTimingReq(MemRequest* pkt, uint64_t currentTime);
    uint64_t recvAtomic(MemRequest* pkt);
    
    void processNextReqEvent(uint32_t channelId, uint64_t currentTime);
    void processRespondEvent(uint32_t channelId, uint64_t currentTime);
    void processAllEventsUpTo(uint64_t time);
    
    bool hasOutstandingRequests() const;
    
    // Queue size query methods - NEW!
    uint32_t readQueueSize() const;           // Total across all channels
    uint32_t readQueueSize(uint32_t channelId) const;  // Specific channel
    uint32_t writeQueueSize() const;          // Total across all channels
    uint32_t writeQueueSize(uint32_t channelId) const; // Specific channel
    
    uint64_t getTotalMemorySize() const { return totalMemorySize; }
    const HBMStats& getStats() const { return stats; }
    uint64_t getNextEventTime() const;
    void printStats();
    
private:
    uint32_t numChannels;
    uint64_t totalMemorySize;
    uint64_t currentTime;
    
    std::vector<HBMInterface*> channels;
    std::vector<std::queue<MemPacket*>> respQueues;
    std::vector<bool> retryRdReq;
    std::vector<bool> retryWrReq;
    
    std::vector<uint8_t> memory;  // Changed from uint8_t* to vector
    
    uint32_t readBufferSize;
    uint32_t writeBufferSize;
    uint32_t writeHighThreshold;
    uint32_t writeLowThreshold;
    uint32_t commandWindow;
    
    InterleaveConfig interleaveConfig;
    
    HBMStats stats;
    
    std::multiset<uint64_t> burstTicks;
    std::multiset<uint64_t> rowBurstTicks;
    std::multiset<uint64_t> colBurstTicks;
    
    uint32_t selectChannel(uint64_t addr) const;
    uint64_t getChannelAddr(uint64_t systemAddr, uint32_t channel) const;
    
    bool readQueueFull(uint32_t channelId, uint32_t neededEntries);
    bool writeQueueFull(uint32_t channelId, uint32_t neededEntries);
    
    void scheduleNextRequest(uint32_t channelId, uint64_t time);
    void scheduleResponse(uint32_t channelId, uint64_t time);
    
    
    
    uint64_t getBurstWindow(uint64_t cmdTick);
    uint64_t verifySingleCmd(uint64_t cmdTick, uint64_t maxCmdsPerBurst, bool rowCmd);
    uint64_t verifyMultiCmd(uint64_t cmdTick, uint64_t maxCmdsPerBurst, uint64_t maxMultiCmdSplit);
    void pruneBurstTick(uint64_t currentTime);
};

} // namespace AcceleratorHBM