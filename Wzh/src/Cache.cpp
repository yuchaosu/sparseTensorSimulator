#include "../include/Cache.h"
#include <iomanip>

// DRAMStorage
void DRAMStorage::store(int groupIndex, const GroupData& data) {
    storage[groupIndex] = data;
}

const GroupData& DRAMStorage::load(int groupIndex) const {
    auto it = storage.find(groupIndex);
    if (it == storage.end()) {
        throw std::out_of_range("DRAM: Group index not found!");
    }
    return it->second;
}

// SetAssociativeCache
SetAssociativeCache::SetAssociativeCache(size_t numSets, size_t waysPerSet)
    : numSets(numSets), waysPerSet(waysPerSet), sets(numSets), hits(0), misses(0) {}

size_t SetAssociativeCache::getSetIndex(int groupIndex) const {
    return groupIndex % numSets;
}

const GroupData* SetAssociativeCache::get(int groupIndex, bool& wasHit) {
    size_t idx = getSetIndex(groupIndex);
    auto& set = sets[idx];
    auto it = set.entries.find(groupIndex);
    //printStats();
    if (it == set.entries.end()) {
        ++misses;
        wasHit = false;
        std::cout << "Cache miss for group index: " << groupIndex << "\n";
        return nullptr;
    } else {
        ++hits;
        wasHit = true;
        // Update LRU
        set.lruList.erase(it->second.second);
        set.lruList.push_front(groupIndex);
        it->second.second = set.lruList.begin();
        return &it->second.first;
    }
}

void SetAssociativeCache::put(int groupIndex, const GroupData& data) {
    size_t idx = getSetIndex(groupIndex);
    auto& set = sets[idx];
    auto it = set.entries.find(groupIndex);

    if (it != set.entries.end()) {
        // Update existing
        set.lruList.erase(it->second.second);
        set.lruList.push_front(groupIndex);
        it->second = {data, set.lruList.begin()};
    } else {
        if (set.entries.size() >= waysPerSet) {
            int lruGroup = set.lruList.back();
            set.lruList.pop_back();
            set.entries.erase(lruGroup);
        }
        set.lruList.push_front(groupIndex);
        set.entries[groupIndex] = {data, set.lruList.begin()};
    }
}

void SetAssociativeCache::printStats() const {
    std::cout << "Cache Hits: " << hits << " Misses: " << misses << "\n";
    //print cache contents
    // std::cout << "+-------+----------------+\n";
    // std::cout << "| Set # |  Group Indices |\n";
    // std::cout << "+-------+----------------+\n";

    // for (size_t i = 0; i < sets.size(); ++i) {
    //     std::cout << "|  " << std::setw(3) << i << "   | ";

    //     if (sets[i].entries.empty()) {
    //         std::cout << "(empty)";
    //     } else {
    //         bool first = true;
    //         for (const auto& entry : sets[i].entries) {
    //             if (!first) std::cout << ", ";
    //             std::cout << entry.first;
    //             first = false;
    //         }
    //     }
    //     std::cout << "\n";
    // }

    // std::cout << "+-------+----------------+\n";
}

void SetAssociativeCache::clear() {
    sets = std::vector<CacheSet>(numSets);
}

// TwoLevelBuffer
TwoLevelBuffer::TwoLevelBuffer(DRAMStorage& dram, SetAssociativeCache& cache)
    : dram(dram), cache(cache) {}

const GroupData& TwoLevelBuffer::get(int groupIndex) {
    bool hit;
    const GroupData* data = cache.get(groupIndex, hit);
    if (hit) {
        return *data;
    } else {
        const GroupData& loaded = dram.load(groupIndex);
        cache.put(groupIndex, loaded);
        return loaded;
    }
}

void TwoLevelBuffer::put(int groupIndex, const GroupData& data) {
    dram.store(groupIndex, data);
    cache.put(groupIndex, data);
}

void TwoLevelBuffer::showCacheStats() const {
    cache.printStats();
}

void TwoLevelBuffer::clear() {
    dram.clear();
    cache.clear();
}
// Scheduler
Scheduler::Scheduler(TwoLevelBuffer& buffer)
    : buffer(buffer) {}

const GroupData& Scheduler::requestGroup(int groupIndex) {
    return buffer.get(groupIndex);
}

void Scheduler::storeGroup(int groupIndex, const GroupData& data) {
    buffer.put(groupIndex, data);
}
