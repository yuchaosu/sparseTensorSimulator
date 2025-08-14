#pragma once
#include <unordered_map>
#include <list>
#include <map>
#include <vector>
#include <tuple>
#include <iostream>

// GroupData: One group of diagonals
using GroupData = std::map<int, std::vector<std::tuple<double, int, int>>>;

// CycleCounter: Tracks cycles spent
class CycleCounter {
private:
    size_t totalCycles = 0;

public:
    void advance(size_t cycles);
    size_t get() const;
    void reset();
};

// DRAMStorage: Simulates DRAM storage with latency
class DRAMStorage {
public: 
    void initialStore(int groupIndex, const GroupData& data); 
    void store(int groupIndex, const GroupData& data, CycleCounter& cycleCounter);
    const GroupData& load(int groupIndex, CycleCounter& cycleCounter) const;
    const GroupData& showload(int groupIndex) const;
    void erase(int groupIndex) {
        storage.erase(groupIndex);
    }
    int size() const;
    std::unordered_map<int, GroupData> getStorage() const;
    void clear();

private:
    std::unordered_map<int, GroupData> storage;
};

// SetAssociativeCache: Cache with set-associative policy and latency
class SetAssociativeCache {
private:
    size_t numSets;
    size_t waysPerSet;

    struct CacheSet {
        std::unordered_map<int, std::pair<GroupData, std::list<int>::iterator>> entries;
        std::list<int> lruList;
    };

    std::vector<CacheSet> sets;
    size_t hits = 0;
    size_t misses = 0;

    size_t getSetIndex(int groupIndex) const;

public:
    SetAssociativeCache(size_t numSets, size_t waysPerSet);

    const GroupData* get(int groupIndex, bool& wasHit, CycleCounter& cycleCounter);
    void put(int groupIndex, const GroupData& data, CycleCounter& cycleCounter);
    void clear();
    std::pair<int, int> printStats() const;
};

// TwoLevelBuffer: Combines DRAM and cache
class TwoLevelBuffer {
private:
    DRAMStorage& dram;
    SetAssociativeCache& cache;
    CycleCounter cycleCounter;

public:
    TwoLevelBuffer(DRAMStorage& dram, SetAssociativeCache& cache);

    const GroupData& get(int groupIndex);
    void put(int groupIndex, const GroupData& data);
    std::pair<int, int> showCacheStats() const;
    void clear();
    size_t getTotalCycles() const;
};

// Scheduler: Requests groups and stores results
class Scheduler {
private:
    TwoLevelBuffer& buffer;

public:
    Scheduler(TwoLevelBuffer& buffer);

    const GroupData& requestGroup(int groupIndex);
    void storeGroup(int groupIndex, const GroupData& data);
    size_t getTotalCycles() const;
};
