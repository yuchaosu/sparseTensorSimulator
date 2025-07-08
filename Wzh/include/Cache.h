#pragma once
#include <unordered_map>
#include <list>
#include <map>
#include <vector>
#include <tuple>
#include <iostream>

// GroupData: One group of diagonals
using GroupData = std::map<int, std::vector<std::tuple<double, int, int>>>;

// DRAMStorage: Simulates DRAM storage
class DRAMStorage {
public:
    void store(int groupIndex, const GroupData& data);
    const GroupData& load(int groupIndex) const;
    const int size() const { return storage.size(); }
    void clear() { storage.clear(); }
private:
    std::unordered_map<int, GroupData> storage;
};

// SetAssociativeCache: Cache with set-associative policy
class SetAssociativeCache {
private:
    size_t numSets;
    size_t waysPerSet;

    struct CacheSet {
        std::unordered_map<int, std::pair<GroupData, std::list<int>::iterator>> entries;
        std::list<int> lruList;
    };

    std::vector<CacheSet> sets;
    size_t hits;
    size_t misses;

    size_t getSetIndex(int groupIndex) const;

public:
    SetAssociativeCache(size_t numSets, size_t waysPerSet);

    const GroupData* get(int groupIndex, bool& wasHit);
    void put(int groupIndex, const GroupData& data);
    void printStats() const;
};

// TwoLevelBuffer: Combines DRAM and cache
class TwoLevelBuffer {
private:
    DRAMStorage& dram;
    SetAssociativeCache& cache;

public:
    TwoLevelBuffer(DRAMStorage& dram, SetAssociativeCache& cache);

    const GroupData& get(int groupIndex);
    void put(int groupIndex, const GroupData& data);
    void showCacheStats() const;
};

// Scheduler: Requests groups and stores results
class Scheduler {
private:
    TwoLevelBuffer& buffer;

public:
    Scheduler(TwoLevelBuffer& buffer);

    const GroupData& requestGroup(int groupIndex);
    void storeGroup(int groupIndex, const GroupData& data);
};
