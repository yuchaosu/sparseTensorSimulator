/*
 * Standalone HBM Controller Implementation
 * Based on gem5 HBM logic but independent implementation
 */

#include "AcceleratorHBM.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstring>

namespace AcceleratorHBM {

// ============================================================================
// HBMInterface Implementation
// ============================================================================

HBMInterface::HBMInterface(uint32_t channelId, const HBMTimingParams& timing)
    : channelId(channelId), timing(timing), controller(nullptr) {
    
    // Configuration for 128-bit interface
    deviceBusWidth = 128;      // bits
    burstLength = 4;           // BL4
    bytesPerBurst = (deviceBusWidth * burstLength) / 8;  // 64 bytes
    banksPerRank = 16;
    bankGroupsPerRank = 4;
    ranksPerChannel = 1;
    rowBufferSize = 2048;      // 2 KiB
    deviceSize = 512ULL * 1024 * 1024;  // 512 MiB per channel
    nextBusFreeAt = 0;
    for (int i = 0; i < 4; ++i) lastIssueAtBG[i] = 0;
    // Initialize bank states
    banks.resize(banksPerRank);
    
    // CRITICAL FIX: Initialize all banks to IDLE with timing constraints at 0
    // This allows immediate activation on first access
    for (auto& bank : banks) {
        bank.state = BankState::IDLE;
        bank.openRow = 0;
        bank.actAllowedAt = 0;   // Allow immediate activation
        bank.preAllowedAt = 0;
        bank.colAllowedAt = 0;
    }
    
    // Initialize timing
    nextBurstAt = 0;
    nextReqTime = 0;
    readsThisTime = 0;
    writesThisTime = 0;
    
    busState = READ;
    busStateNext = READ;
    
    rangeStart = 0;
    rangeEnd = 0;
}

HBMInterface::~HBMInterface() {
    // Clean up queues
    for (auto pkt : readQueue) delete pkt;
    for (auto pkt : writeQueue) delete pkt;
}

void HBMInterface::setAddrRange(uint64_t start, uint64_t size) {
    rangeStart = start;
    rangeEnd = start + size;
}

void HBMInterface::setController(void* ctrl) {
    controller = ctrl;
}

uint32_t HBMInterface::bitsNeeded(uint64_t value) {
    if (value == 0) return 1;
    return static_cast<uint32_t>(std::ceil(std::log2(value + 1)));
}

uint64_t HBMInterface::extractBits(uint64_t addr, uint32_t lsb, uint32_t width) {
    uint64_t mask = (1ULL << width) - 1;
    return (addr >> lsb) & mask;
}

void HBMInterface::decodeAddress(uint64_t addr, uint8_t& rank, uint8_t& bank,
                                 uint16_t& row, uint16_t& col, uint8_t& bankGroup) {
    // Address mapping for interleaved memory:
    // We need to remove the channel selection bits first to get the local address
    // This is handled by the controller before calling this function
    
    // For now, use a simplified mapping: [row | bank_group | bank | column | burst]
    uint32_t burstBits = bitsNeeded(bytesPerBurst - 1);
    uint32_t colBits = bitsNeeded(rowBufferSize / bytesPerBurst - 1);
    uint32_t bankBits = bitsNeeded(banksPerRank / bankGroupsPerRank - 1);  // Banks within group
    uint32_t bankGroupBits = bitsNeeded(bankGroupsPerRank - 1);
    
    // Use the full address (controller routes to correct channel)
    uint64_t localAddr = addr;
    
    // Extract fields from LSB to MSB
    uint32_t offset = burstBits;
    col = extractBits(localAddr, offset, colBits);
    offset += colBits;
    
    uint8_t bankInGroup = extractBits(localAddr, offset, bankBits);
    offset += bankBits;
    
    bankGroup = extractBits(localAddr, offset, bankGroupBits);
    offset += bankGroupBits;
    
    // Combine bank group and bank within group
    bank = (bankGroup * (banksPerRank / bankGroupsPerRank)) + bankInGroup;
    
    row = extractBits(localAddr, offset, 16);  // Assume 16-bit row address
    
    rank = 0;  // Single rank per channel
}

uint64_t HBMInterface::burstAlign(uint64_t addr) {
    return addr & ~(uint64_t)(bytesPerBurst - 1);
}

bool HBMInterface::canIssueActivate(uint8_t bank, uint64_t currentTime) {
    return (banks[bank].state == BankState::IDLE &&
            banks[bank].actAllowedAt <= currentTime);
}

bool HBMInterface::canIssuePrecharge(uint8_t bank, uint64_t currentTime) {
    return (banks[bank].state == BankState::ACTIVE &&
            banks[bank].preAllowedAt <= currentTime);
}

bool HBMInterface::canIssueColumn(uint8_t bank, uint64_t currentTime) {
    return (banks[bank].state == BankState::ACTIVE &&
            banks[bank].colAllowedAt <= currentTime);
}

bool HBMInterface::isRowHit(uint8_t bank, uint16_t row) {
    return (banks[bank].state == BankState::ACTIVE &&
            banks[bank].openRow == row);
}

uint64_t HBMInterface::scheduleActivate(uint8_t bank, uint16_t row, uint64_t currentTime) {
    // Issue activate command
    uint64_t actTime = std::max(currentTime, banks[bank].actAllowedAt);
    
    banks[bank].state = BankState::ACTIVE;
    banks[bank].openRow = row;
    
    // Update timing constraints
    banks[bank].colAllowedAt = actTime + static_cast<uint64_t>(timing.tRCD);
    banks[bank].preAllowedAt = actTime + static_cast<uint64_t>(timing.tRAS);
    
    // Next activate to same bank
    banks[bank].actAllowedAt = actTime + static_cast<uint64_t>(timing.tRC());
    
    return actTime;
}

uint64_t HBMInterface::schedulePrecharge(uint8_t bank, uint64_t currentTime) {
    // Issue precharge command
    uint64_t preTime = std::max(currentTime, banks[bank].preAllowedAt);
    
    banks[bank].state = BankState::IDLE;
    
    // Update timing constraints
    banks[bank].actAllowedAt = preTime + static_cast<uint64_t>(timing.tRP);
    
    return preTime;
}

uint64_t HBMInterface::scheduleColumn(uint8_t bank, bool isRead, uint64_t currentTime) {
    // Issue column command (read or write)
    uint64_t colTime = std::max(currentTime, banks[bank].colAllowedAt);
    
    // Calculate when data will be ready
    uint64_t dataTime;
    if (isRead) {
        dataTime = colTime + static_cast<uint64_t>(timing.tCL + timing.tBURST);
        // Update precharge constraint
        banks[bank].preAllowedAt = std::max(banks[bank].preAllowedAt,
                                            colTime + static_cast<uint64_t>(timing.tRTP));
    } else {
        dataTime = colTime + static_cast<uint64_t>(timing.tCWL + timing.tBURST);
        // Update precharge constraint
        banks[bank].preAllowedAt = std::max(banks[bank].preAllowedAt,
                                            colTime + static_cast<uint64_t>(timing.tCWL + timing.tBURST + timing.tWR));
    }
    
    // Update column-to-column timing
    banks[bank].colAllowedAt = colTime + static_cast<uint64_t>(timing.tCCD_L);
    
    return dataTime;
}

uint64_t HBMInterface::doAccess(MemPacket* pkt, uint64_t currentTime) {
    uint8_t rank, bank, bankGroup;
    uint16_t row, col;
    decodeAddress(pkt->addr, rank, bank, row, col, bankGroup);

    uint64_t cmdTime = currentTime;

    // ----- Row policy (keep your existing behavior) -----
    if (!isRowHit(bank, row)) {
        if (banks[bank].state == BankState::ACTIVE) {
            cmdTime = schedulePrecharge(bank, cmdTime);
        }
        cmdTime = scheduleActivate(bank, row, cmdTime);
    }

    // ----- Idealized column pipeline -----
    // Per-BG spacing: next CAS in the same BG must honor tCCD_L,
    // different BGs can go as fast as tCCD_S.
    const uint64_t lastBG = lastIssueAtBG[bankGroup];

    // Earliest time allowed by BG spacing:
    // If we issued in this BG before, enforce tCCD_L;
    // If this is the first issue, lastBG==0 and max() with cmdTime dominates.
    uint64_t earliestBG = (lastBG == 0) ? cmdTime
                                        : std::max(cmdTime, lastBG + (uint64_t)timing.tCCD_L);

    // Channel data bus token: enforce one burst every tBURST
    // (i.e., bus can't carry two bursts simultaneously).
    uint64_t earliestBus = std::max(earliestBG, nextBusFreeAt);

    // Issue at the max of all constraints
    uint64_t issueTime = std::max(earliestBG, nextBusFreeAt);

    // Data ready after tCL + tBURST from issue
    uint64_t readyTime = issueTime + (uint64_t)timing.tCL + (uint64_t)timing.tBURST;

    // Update trackers:
    lastIssueAtBG[bankGroup] = issueTime;        // per-BG CAS time
    nextBusFreeAt            = issueTime + (uint64_t)timing.tBURST; // bus occupied for tBURST

    return readyTime;
}


void HBMInterface::addToReadQueue(MemPacket* pkt) {
    readQueue.push_back(pkt);
}

void HBMInterface::addToWriteQueue(MemPacket* pkt) {
    writeQueue.push_back(pkt);
}

MemPacket* HBMInterface::chooseNextRead(uint64_t currentTime) {
    // Simple FCFS scheduling - find first ready packet
    for (auto it = readQueue.begin(); it != readQueue.end(); ++it) {
        MemPacket* pkt = *it;
        uint8_t rank, bank, bankGroup;
        uint16_t row, col;
        decodeAddress(pkt->addr, rank, bank, row, col, bankGroup);
        
        // Check if we can access this packet:
        // 1. Row hit - can access immediately
        // 2. Bank is IDLE - can activate immediately  
        // 3. Bank is ACTIVE with wrong row - can precharge then activate
        if (isRowHit(bank, row)) {
            readQueue.erase(it);
            return pkt;
        }
        
        if (banks[bank].state == BankState::IDLE && canIssueActivate(bank, currentTime)) {
            readQueue.erase(it);
            return pkt;
        }
        
        // If bank is ACTIVE with different row, check if we can precharge
        if (banks[bank].state == BankState::ACTIVE && canIssuePrecharge(bank, currentTime)) {
            // We can precharge and then activate - doAccess() will handle this
            readQueue.erase(it);
            return pkt;
        }
    }
    return nullptr;
}

MemPacket* HBMInterface::chooseNextWrite(uint64_t currentTime) {
    // Simple FCFS scheduling
    for (auto it = writeQueue.begin(); it != writeQueue.end(); ++it) {
        MemPacket* pkt = *it;
        uint8_t rank, bank, bankGroup;
        uint16_t row, col;
        decodeAddress(pkt->addr, rank, bank, row, col, bankGroup);
        
        // Check if we can access this packet:
        // 1. Row hit - can access immediately
        // 2. Bank is IDLE - can activate immediately
        // 3. Bank is ACTIVE with wrong row - can precharge then activate
        if (isRowHit(bank, row)) {
            writeQueue.erase(it);
            return pkt;
        }
        
        if (banks[bank].state == BankState::IDLE && canIssueActivate(bank, currentTime)) {
            writeQueue.erase(it);
            return pkt;
        }
        
        // If bank is ACTIVE with different row, check if we can precharge
        if (banks[bank].state == BankState::ACTIVE && canIssuePrecharge(bank, currentTime)) {
            // We can precharge and then activate - doAccess() will handle this
            writeQueue.erase(it);
            return pkt;
        }
    }
    return nullptr;
}

// ============================================================================
// HBMController Implementation
// ============================================================================

HBMController::HBMController(uint32_t numChannels)
    : numChannels(numChannels), totalMemorySize(0), currentTime(0) {
    
    // Configuration
    readBufferSize = 128;   // Per channel
    writeBufferSize = 128;  // Per channel
    writeHighThreshold = (writeBufferSize * 80) / 100;  // 80%
    writeLowThreshold = (writeBufferSize * 50) / 100;   // 50%
    commandWindow = 10;  // 10ns window for command scheduling
    
    // Default interleaving: cache line (64 bytes)
    interleaveConfig = InterleaveConfig();
    
    // Create channels
    HBMTimingParams timing;
    for (uint32_t i = 0; i < numChannels; ++i) {
        channels.push_back(new HBMInterface(i, timing));
        respQueues.push_back(std::queue<MemPacket*>());
        retryRdReq.push_back(false);
        retryWrReq.push_back(false);
    }
    
    // Initialize per-channel stats
    stats.channelAccesses.resize(numChannels, 0);
}

HBMController::HBMController(uint32_t numChannels, const InterleaveConfig& config)
    : numChannels(numChannels), totalMemorySize(0), interleaveConfig(config), currentTime(0) {
    
    // Configuration
    readBufferSize = 128;
    writeBufferSize = 128;
    writeHighThreshold = (writeBufferSize * 80) / 100;
    writeLowThreshold = (writeBufferSize * 50) / 100;
    commandWindow = 10;
    
    // Create channels
    HBMTimingParams timing;
    for (uint32_t i = 0; i < numChannels; ++i) {
        channels.push_back(new HBMInterface(i, timing));
        respQueues.push_back(std::queue<MemPacket*>());
        retryRdReq.push_back(false);
        retryWrReq.push_back(false);
    }
    
    stats.channelAccesses.resize(numChannels, 0);
}

HBMController::~HBMController() {
    for (auto channel : channels) {
        delete channel;
    }
}

void HBMController::init(uint64_t totalSize) {
    totalMemorySize = totalSize;
    
    // With interleaving, ALL channels cover the FULL address range
    // The channel selection is done by extracting bits from the address
    // Each channel is responsible for a subset of addresses based on interleaving
    for (uint32_t i = 0; i < numChannels; ++i) {
        channels[i]->setAddrRange(0, totalMemorySize);
        channels[i]->setController(this);
    }
    
    // Allocate physical memory
    memory.resize(totalMemorySize, 0);
}

void HBMController::setInterleaveConfig(const InterleaveConfig& config) {
    interleaveConfig = config;
}

void HBMController::printInterleaveConfig() const {
    std::cout << "\n=== Interleaving Configuration ===" << std::endl;
    std::cout << "Scheme: ";
    switch (interleaveConfig.scheme) {
        case InterleavingScheme::CACHE_LINE:
            std::cout << "Cache Line (64 bytes)" << std::endl;
            break;
        case InterleavingScheme::PAGE:
            std::cout << "Page (4 KB)" << std::endl;
            break;
        case InterleavingScheme::CUSTOM:
            std::cout << "Custom" << std::endl;
            break;
    }
    std::cout << "Interleave size: " << interleaveConfig.interleaveSize << " bytes" << std::endl;
    std::cout << "Interleave bits: " << interleaveConfig.interleaveBits 
              << " (for " << numChannels << " channels)" << std::endl;
    std::cout << "Low bit position: " << interleaveConfig.interleaveLowBit << std::endl;
    std::cout << "Address bits [" << (interleaveConfig.interleaveLowBit + interleaveConfig.interleaveBits - 1)
              << ":" << interleaveConfig.interleaveLowBit << "] select channel" << std::endl;
    std::cout << "==================================\n" << std::endl;
}

uint32_t HBMController::selectChannel(uint64_t addr) const {
    // Extract channel bits based on interleaving configuration
    uint64_t mask = (1ULL << interleaveConfig.interleaveBits) - 1;
    uint32_t channel = (addr >> interleaveConfig.interleaveLowBit) & mask;
    return channel % numChannels;
}

uint64_t HBMController::getChannelAddr(uint64_t systemAddr, uint32_t channel) const {
    // Convert system address to channel-local address
    // Remove the interleaving bits and calculate offset within channel
    
    uint64_t interleaveMask = (1ULL << interleaveConfig.interleaveLowBit) - 1;
    uint64_t lowerBits = systemAddr & interleaveMask;
    
    uint64_t upperAddr = systemAddr >> (interleaveConfig.interleaveLowBit + interleaveConfig.interleaveBits);
    uint64_t channelAddr = (upperAddr << interleaveConfig.interleaveLowBit) | lowerBits;
    
    return channelAddr;
}

void HBMController::startup(uint64_t currentTime) {
    // Initialize timing for all channels
    for (auto channel : channels) {
        channel->nextBurstAt = currentTime + 10;  // Small offset
    }
}

bool HBMController::readQueueFull(uint32_t channelId, uint32_t neededEntries) {
    auto& channel = channels[channelId];
    uint32_t totalSize = channel->readQueue.size() + respQueues[channelId].size();
    return (totalSize + neededEntries) > readBufferSize;
}

bool HBMController::writeQueueFull(uint32_t channelId, uint32_t neededEntries) {
    auto& channel = channels[channelId];
    return (channel->writeQueue.size() + neededEntries) > writeBufferSize;
}

bool HBMController::recvTimingReq(MemRequest* pkt, uint64_t currentTime) {
    // Select channel based on address INTERNALLY - test code shouldn't know about channels
    uint32_t channelId = selectChannel(pkt->addr);
    auto& channel = channels[channelId];
    
    // Track per-channel accesses
    stats.channelAccesses[channelId]++;
    
    // Check if address is in valid range (within total memory)
    if (pkt->addr >= totalMemorySize) {
        std::cerr << "Address 0x" << std::hex << pkt->addr 
                  << " out of range (max: 0x" << totalMemorySize << ")" << std::dec << std::endl;
        return false;
    }
    
    // Calculate number of memory packets needed
    uint32_t burstSize = channel->getBytesPerBurst();
    uint32_t offset = pkt->addr & (burstSize - 1);
    uint32_t pktCount = (offset + pkt->size + burstSize - 1) / burstSize;
    
    // Check if queues are full
    if (pkt->isWrite) {
        if (writeQueueFull(channelId, pktCount)) {
            retryWrReq[channelId] = true;
            return false;
        }
    } else if (pkt->isRead) {
        if (readQueueFull(channelId, pktCount)) {
            retryRdReq[channelId] = true;
            return false;
        }
    }
    
    // Split packet into memory packets if needed
    uint64_t addr = pkt->addr;
    for (uint32_t i = 0; i < pktCount; ++i) {
        MemPacket* memPkt = new MemPacket();
        memPkt->addr = channel->burstAlign(addr);
        memPkt->size = std::min(burstSize, pkt->size - i * burstSize);
        memPkt->isRead = pkt->isRead;
        memPkt->entryTime = currentTime;
        memPkt->qosValue = pkt->qosValue;
        memPkt->requestorId = pkt->requestorId;
        memPkt->pkt = pkt;
        
        if (pkt->isRead) {
            channel->addToReadQueue(memPkt);
            stats.readBursts++;
        } else {
            channel->addToWriteQueue(memPkt);
            stats.writeBursts++;
        }
        
        addr += burstSize;
    }
    
    // // Update statistics
    // if (pkt->isRead) {
    //     stats.readReqs++;
    //     stats.bytesRead += pkt->size;
    // } else {
    //     stats.writeReqs++;
    //     stats.bytesWritten += pkt->size;
    // }
    
    // INTERNALLY process the request - the selected channel is hidden from caller
    processNextReqEvent(channelId, currentTime);
    
    return true;
}

uint64_t HBMController::recvAtomic(MemRequest* pkt) {
    // INTERNALLY select the channel; caller stays oblivious to channels
    uint32_t channelId = selectChannel(pkt->addr);
    auto& channel = channels[channelId];

    // Track access for statistics (per request)
    stats.channelAccesses[channelId]++;

    // Guard: in-range
    if (pkt->addr >= totalMemorySize) {
        std::cerr << "Address 0x" << std::hex << pkt->addr
                  << " out of range (max: 0x" << totalMemorySize << ")"
                  << std::dec << std::endl;
        return 0;
    }

    // Split into 64B bursts (device granularity)
    const uint32_t burstSize = channel->getBytesPerBurst();
    const uint32_t offset    = pkt->addr & (burstSize - 1);
    const uint32_t pktCount  = (offset + pkt->size + burstSize - 1) / burstSize;

    // Seed time for this channel (keeps atomic accesses serialized per channel)
    uint64_t startTime = channel->getNextBurstAt();   // current channel time base
    uint64_t lastReady = startTime;

    // Walk bursts using the real timing path
    uint64_t addr = pkt->addr;
    for (uint32_t i = 0; i < pktCount; ++i) {
        MemPacket* mp   = new MemPacket();
        mp->addr        = channel->burstAlign(addr);
        mp->size        = std::min<uint32_t>(burstSize, pkt->size - i * burstSize);
        mp->isRead      = pkt->isRead;
        mp->entryTime   = startTime;   // used for stats accounting if needed
        mp->qosValue    = pkt->qosValue;
        mp->requestorId = pkt->requestorId;
        mp->pkt         = pkt;

        // Use the channel's current availability as "now"
        uint64_t ready = channel->doAccess(mp, lastReady);
        mp->readyTime  = ready;
        lastReady      = ready;

        // We don't need to enqueue/resp—atomic returns immediately.
        delete mp;
        addr += burstSize;
    }

    // Apply the data move atomically at the end (functional model)
    if (pkt->isWrite && pkt->data) {
        std::memcpy(&memory[pkt->addr], pkt->data, pkt->size);
        //stats.writeReqs++;
        //stats.bytesWritten += pkt->size;
    } else if (pkt->isRead && pkt->data) {
        std::memcpy(pkt->data, &memory[pkt->addr], pkt->size);
        //stats.readReqs++;
        //stats.bytesRead += pkt->size;
    }

    // Latency seen by the caller is the time to the last burst’s data
    // measured from the original channel time base.
    uint64_t latency = (lastReady >= startTime) ? (lastReady - startTime) : 0;
    return latency;
}

void HBMController::processNextReqEvent(uint32_t channelId, uint64_t currentTime) {
    auto& channel = channels[channelId];
    
    // Determine if we should switch between read and write
    bool switchToWrites = false;
    bool switchToReads = false;
    
    if (channel->busState == HBMInterface::READ) {
        // Check if should switch to writes
        if (channel->readQueue.empty()) {
            if (!channel->writeQueue.empty()) {
                switchToWrites = true;
            } else {
                return;  // Nothing to do
            }
        }
        
        // Check write pressure
        if (channel->writeQueue.size() > writeHighThreshold) {
            switchToWrites = true;
        }
    } else {  // WRITE state
        // Check if should switch to reads
        if (channel->writeQueue.empty()) {
            if (!channel->readQueue.empty()) {
                switchToReads = true;
            } else {
                return;
            }
        }
        
        // Check if below threshold and reads waiting
        if (channel->writeQueue.size() < writeLowThreshold && !channel->readQueue.empty()) {
            switchToReads = true;
        }
    }
    
    // Update bus state
    if (switchToWrites) {
        channel->busStateNext = HBMInterface::WRITE;
        channel->busState = HBMInterface::WRITE;
    } else if (switchToReads) {
        channel->busStateNext = HBMInterface::READ;
        channel->busState = HBMInterface::READ;
    }
    
    // Process request based on bus state
    MemPacket* pkt = nullptr;
    if (channel->busState == HBMInterface::READ && !channel->readQueue.empty()) {
        pkt = channel->chooseNextRead(currentTime);
        if (pkt) {
            channel->readsThisTime++;
        }
    } else if (channel->busState == HBMInterface::WRITE && !channel->writeQueue.empty()) {
        pkt = channel->chooseNextWrite(currentTime);
        if (pkt) {
            channel->writesThisTime++;
        }
    }
    
    if (pkt) {
        // Perform the access
        uint64_t readyTime = channel->doAccess(pkt, currentTime);
        pkt->readyTime = readyTime;
        
        // Add to response queue if read
        if (pkt->isRead) {
            respQueues[channelId].push(pkt);
            scheduleResponse(channelId, readyTime);
            
            // Update latency stats
            stats.totalReadLatency += (readyTime - pkt->entryTime);
        } else {
            // Write completes immediately (posted write)
            // FIX: Increment stats counters for writes
            stats.writeReqs++;
            stats.bytesWritten += pkt->size;
            stats.totalWriteLatency += (currentTime - pkt->entryTime);
            
            // Perform actual write to memory
            if (pkt->pkt && pkt->pkt->data) {
                std::memcpy(&memory[pkt->addr], pkt->pkt->data, pkt->size);
            }
            
            delete pkt;
        }
        
        // Schedule next request
        scheduleNextRequest(channelId, channel->nextBurstAt);
    }
}

void HBMController::processRespondEvent(uint32_t channelId, uint64_t currentTime) {
    auto& respQueue = respQueues[channelId];
    if (respQueue.empty()) return;

    MemPacket* pkt = respQueue.front();

    // If the packet has reached its ready time — complete it
    if (pkt->readyTime <= currentTime) {

        // Perform memory read/write to backing store
        if (pkt->pkt && pkt->pkt->data) {
            if (pkt->isRead) {
                std::memcpy(pkt->pkt->data, &memory[pkt->addr], pkt->size);
            } else {
                std::memcpy(&memory[pkt->addr], pkt->pkt->data, pkt->size);
            }
        }

        // Update statistics
        if (pkt->isRead) {
            stats.readReqs++;
            stats.bytesRead += pkt->size;
        } else {
            stats.writeReqs++;
            stats.bytesWritten += pkt->size;
        }
        stats.channelAccesses[channelId]++;

        // Retire the request
        respQueue.pop();
        delete pkt;

        // Schedule next response if any remain
        if (!respQueue.empty()) {
            scheduleResponse(channelId, respQueue.front()->readyTime);
        }

    } else {
        // Not ready yet → reschedule next check to its readyTime
        scheduleResponse(channelId, pkt->readyTime);
    }
}



void HBMController::scheduleNextRequest(uint32_t channelId, uint64_t time) {
    // In a real event-driven system, this would schedule an event
    // For this standalone implementation, we track when next processing should happen
    channels[channelId]->nextReqTime = time;
}

void HBMController::scheduleResponse(uint32_t channelId, uint64_t time) {
    // In a real event-driven system, this would schedule a response event
    // For standalone, we track the response time
    if (!respQueues[channelId].empty()) {
        respQueues[channelId].front()->readyTime = time;
    }
}

uint64_t HBMController::getNextEventTime() const {
    // Find the earliest event time across all channels
    uint64_t minTime = UINT64_MAX;
    
    // Check next request times
    for (auto& channel : channels) {
        if (channel->nextReqTime < minTime) {
            minTime = channel->nextReqTime;
        }
    }
    
    // Check response queue ready times
    for (const auto& respQueue : respQueues) {
        if (!respQueue.empty() && respQueue.front()->readyTime < minTime) {
            minTime = respQueue.front()->readyTime;
        }
    }
    
    return minTime;
}

bool HBMController::hasOutstandingRequests() const {
    // Check if any channel has pending work
    for (auto& channel : channels) {
        if (!channel->readQueue.empty() || !channel->writeQueue.empty()) {
            return true;
        }
    }
    
    for (const auto& respQueue : respQueues) {
        if (!respQueue.empty()) {
            return true;
        }
    }
    
    return false;
}

uint64_t HBMController::getBurstWindow(uint64_t cmdTick) {
    uint64_t burstOffset = cmdTick % commandWindow;
    return cmdTick - burstOffset;
}

uint64_t HBMController::verifySingleCmd(uint64_t cmdTick, uint64_t maxCmdsPerBurst, bool rowCmd) {
    uint64_t cmdAt = cmdTick;
    uint64_t burstTick = getBurstWindow(cmdTick);
    
    auto& ticks = rowCmd ? rowBurstTicks : colBurstTicks;
    
    while (ticks.count(burstTick) >= maxCmdsPerBurst) {
        burstTick += commandWindow;
        cmdAt = burstTick;
    }
    
    ticks.insert(burstTick);
    return cmdAt;
}

uint64_t HBMController::verifyMultiCmd(uint64_t cmdTick, uint64_t maxCmdsPerBurst,
                                      uint64_t maxMultiCmdSplit) {
    uint64_t cmdAt = cmdTick;
    uint64_t burstTick = getBurstWindow(cmdTick);
    
    uint64_t burstOffset = 0;
    uint64_t firstCmdOffset = cmdTick % commandWindow;
    while (maxMultiCmdSplit > (firstCmdOffset + burstOffset)) {
        burstOffset += commandWindow;
    }
    
    uint64_t firstCmdTick = (burstTick >= burstOffset) ? (burstTick - burstOffset) : 0;
    
    bool firstCanIssue = false;
    bool secondCanIssue = false;
    
    while (!firstCanIssue || !secondCanIssue) {
        bool sameBurst = (burstTick == firstCmdTick);
        auto firstCmdCount = rowBurstTicks.count(firstCmdTick);
        auto secondCmdCount = sameBurst ? firstCmdCount + 1 : rowBurstTicks.count(burstTick);
        
        firstCanIssue = firstCmdCount < maxCmdsPerBurst;
        secondCanIssue = secondCmdCount < maxCmdsPerBurst;
        
        if (!secondCanIssue) {
            burstTick += commandWindow;
            cmdAt = burstTick;
        }
        
        bool gapViolated = !sameBurst && ((burstTick - firstCmdTick) > maxMultiCmdSplit);
        
        if (!firstCanIssue || (!secondCanIssue && gapViolated)) {
            firstCmdTick += commandWindow;
        }
    }
    
    rowBurstTicks.insert(burstTick);
    rowBurstTicks.insert(firstCmdTick);
    
    return cmdAt;
}

void HBMController::pruneBurstTick(uint64_t currentTime) {
    uint64_t window = getBurstWindow(currentTime);
    
    auto it = burstTicks.begin();
    while (it != burstTicks.end()) {
        if (window > *it) {
            it = burstTicks.erase(it);
        } else {
            ++it;
        }
    }
    
    it = rowBurstTicks.begin();
    while (it != rowBurstTicks.end()) {
        if (window > *it) {
            it = rowBurstTicks.erase(it);
        } else {
            ++it;
        }
    }
    
    it = colBurstTicks.begin();
    while (it != colBurstTicks.end()) {
        if (window > *it) {
            it = colBurstTicks.erase(it);
        } else {
            ++it;
        }
    }
}

void HBMController::printStats() {
    std::cout << "\n=== HBM Controller Statistics ===" << std::endl;
    std::cout << "Read Requests:  " << stats.readReqs << std::endl;
    std::cout << "Write Requests: " << stats.writeReqs << std::endl;
    std::cout << "Read Bursts:    " << stats.readBursts << std::endl;
    std::cout << "Write Bursts:   " << stats.writeBursts << std::endl;
    std::cout << "Bytes Read:     " << stats.bytesRead << std::endl;
    std::cout << "Bytes Written:  " << stats.bytesWritten << std::endl;
    
    if (stats.readReqs > 0) {
        double avgReadLat = static_cast<double>(stats.totalReadLatency) / stats.readReqs;
        std::cout << "Avg Read Latency: " << avgReadLat << " ns" << std::endl;
    }
    
    if (stats.writeReqs > 0) {
        double avgWriteLat = static_cast<double>(stats.totalWriteLatency) / stats.writeReqs;
        std::cout << "Avg Write Latency: " << avgWriteLat << " ns" << std::endl;
    }
    
    // Print per-channel statistics
    std::cout << "\nPer-Channel Access Distribution:" << std::endl;
    uint64_t totalAccesses = 0;
    for (uint32_t i = 0; i < numChannels; ++i) {
        totalAccesses += stats.channelAccesses[i];
    }
    
    for (uint32_t i = 0; i < numChannels; ++i) {
        double percentage = totalAccesses > 0 ? 
            (static_cast<double>(stats.channelAccesses[i]) / totalAccesses * 100.0) : 0.0;
        std::cout << "  Channel " << i << ": " << stats.channelAccesses[i] 
                  << " accesses (" << std::fixed << std::setprecision(2) 
                  << percentage << "%)" << std::endl;
    }
    
    std::cout << "==================================\n" << std::endl;
}

} // namespace AcceleratorHBM