#ifndef HBM_H
#define HBM_H

#include <cstdint>
#include <queue>
#include <vector>
#include <functional>
#include <string>
#include <cstdio>

// Memory request structure
struct MemoryRequest {
    uint64_t address;
    bool isWrite;
    uint64_t arrivalTime;
    uint64_t size;
    int requestId;
    
    MemoryRequest(uint64_t addr, bool write, uint64_t time, uint64_t sz, int id)
        : address(addr), isWrite(write), arrivalTime(time), size(sz), requestId(id) {}
};

// Bank state
struct Bank {
    enum State { IDLE, ACTIVE, PRECHARGING, REFRESHING };
    State state;
    uint64_t activeRow;
    uint64_t nextAvailableTime;
    uint64_t prechargeAllowedTime;
    
    Bank() : state(IDLE), activeRow(0), nextAvailableTime(0), prechargeAllowedTime(0) {}
};

// HBM Channel
class HBMChannel {
private:
    static constexpr int BANKS_PER_RANK = 8;
    static constexpr int DEVICE_BUS_WIDTH = 128;
    static constexpr int BURST_LENGTH = 4;
    static constexpr size_t CHANNEL_SIZE = 128 * 1024 * 1024; // 128MiB
    static constexpr size_t ROW_BUFFER_SIZE = 2048; // 2KiB
    static constexpr size_t MAX_QUEUE_SIZE = 128;
    
    // Timing parameters (in ns)
    static constexpr uint64_t tCK = 1;
    static constexpr uint64_t tRP = 11;
    static constexpr uint64_t tRCD = 11;
    static constexpr uint64_t tCL = 10;
    static constexpr uint64_t tRAS = 28;
    static constexpr uint64_t tBURST = 4;
    static constexpr uint64_t tRFC = 130;
    static constexpr uint64_t tREFI = 7800;
    static constexpr uint64_t tWR = 12;
    static constexpr uint64_t tRTP = 6;
    static constexpr uint64_t tWTR = 7;
    static constexpr uint64_t tRTW = 3;
    static constexpr uint64_t tRRD = 3;
    static constexpr uint64_t tXAW = 24;
    static constexpr uint64_t tXP = 6;
    static constexpr uint64_t tXS = 140;
    
    std::vector<Bank> banks;
    std::vector<MemoryRequest> readQueue;
    std::vector<MemoryRequest> writeQueue;
    uint64_t currentTime;
    uint64_t nextRefreshTime;
    uint64_t busAvailableTime;
    uint64_t lastActivationTime[4]; // For tXAW tracking
    int activationIndex;
    uint64_t dataBusAvailableTime;
    uint64_t lastCommandDataTime;
    bool lastCommandWasWrite;
    uint64_t lastActivateAnyBank;
    
    // Address mapping
    uint64_t getBank(uint64_t address);
    uint64_t getRow(uint64_t address);
    uint64_t getColumn(uint64_t address);
    
    // FR-FCFS scheduling
    MemoryRequest* selectRequest(std::vector<MemoryRequest>& queue);
    bool isRowHit(const MemoryRequest& req, int bank);
    MemoryRequest* selectBestRequest();
    void processQueues();
    void performRefresh();
    uint64_t serviceRequestInternal(const MemoryRequest& req);
    uint64_t bytesPerBurst() const;
    uint64_t findNextArrivalTime() const;
    
public:
    HBMChannel();
    void tick(uint64_t time);
    void addRequest(const MemoryRequest& req);
    bool canAccept();
    uint64_t getLatency(const MemoryRequest& req);
    uint64_t getCurrentTime() const { return currentTime; }
    uint64_t getCompletionTime() const { return std::max(currentTime, dataBusAvailableTime); }
    bool hasPendingRequests() const;
};

// HBM Controller with 8 channels
class HBMController {
private:
    static constexpr int NUM_CHANNELS = 8;
    std::vector<HBMChannel> channels;
    uint64_t currentTime;
    
    // Interleaved address mapping
    int getChannel(uint64_t address);
    uint64_t getChannelAddress(uint64_t address);
    
public:
    HBMController();
    void tick();
    void addRequest(uint64_t address, bool isWrite, uint64_t size, int requestId);
    uint64_t getCurrentTime() const { return currentTime; }
    bool hasPendingRequests() const;
    uint64_t getMaxChannelTime() const;

    // Tracing API
    void enableTrace(const std::string& path);
    void disableTrace();
    // Aggregate tracing API (compact summary instead of per-event lines)
    void enableTraceAggregate(const std::string& path);
    void disableTraceAggregate();
    bool isTraceEnabled() const { return traceEnabled; }

    // Tracing
    bool traceEnabled = false;
    FILE* traceFp = nullptr;
    // Aggregate tracing
    bool traceAggregateEnabled = false;
    FILE* traceAggregateFp = nullptr;
    uint64_t agg_total_bytes = 0;
    uint64_t agg_enq_count = 0;
    uint64_t agg_comp_count = 0;
    uint64_t agg_first_ts = 0;
    uint64_t agg_last_ts = 0;
};

#endif // HBM_H