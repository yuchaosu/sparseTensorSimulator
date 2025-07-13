#include "../include/Cache.h"
#include <stdexcept>

// =============================
// CycleCounter
// =============================

void CycleCounter::advance(size_t cycles) {
    totalCycles += cycles;
}

size_t CycleCounter::get() const {
    return totalCycles;
}

void CycleCounter::reset() {
    totalCycles = 0;
}

// =============================
// DRAMStorage
// =============================
void DRAMStorage::initialStore(int groupIndex, const GroupData& data) {
    storage[groupIndex] = data;
}

void DRAMStorage::store(int groupIndex, const GroupData& data, CycleCounter& cycleCounter) {
    storage[groupIndex] = data;
    cycleCounter.advance(20); // DRAM write latency
}

const GroupData& DRAMStorage::load(int groupIndex, CycleCounter& cycleCounter) const {
    auto it = storage.find(groupIndex);
    if (it == storage.end()) {
        throw std::runtime_error("DRAM load: group not found");
    }
    cycleCounter.advance(20); // DRAM read latency
    return it->second;
}

const GroupData& DRAMStorage::showload(int groupIndex) const {
    auto it = storage.find(groupIndex);
    if (it == storage.end()) {
        throw std::runtime_error("DRAM showload: group not found");
    }
    return it->second;
}

int DRAMStorage::size() const {
    return static_cast<int>(storage.size());
}

std::unordered_map<int, GroupData> DRAMStorage::getStorage() const {
    return storage;
}

void DRAMStorage::clear() {
    storage.clear();
}

// =============================
// SetAssociativeCache
// =============================

SetAssociativeCache::SetAssociativeCache(size_t numSets_, size_t waysPerSet_)
    : numSets(numSets_), waysPerSet(waysPerSet_), sets(numSets_) {}

size_t SetAssociativeCache::getSetIndex(int groupIndex) const {
    return static_cast<size_t>(groupIndex) % numSets;
}

const GroupData* SetAssociativeCache::get(int groupIndex, bool& wasHit, CycleCounter& cycleCounter) {
    size_t setIdx = getSetIndex(groupIndex);
    auto& set = sets[setIdx];

    auto it = set.entries.find(groupIndex);
    if (it != set.entries.end()) {
        // HIT
        wasHit = true;
        hits++;
        set.lruList.erase(it->second.second);
        set.lruList.push_front(groupIndex);
        it->second.second = set.lruList.begin();
        cycleCounter.advance(1); // Cache hit latency
        return &(it->second.first);
    } else {
        // MISS
        wasHit = false;
        misses++;
        cycleCounter.advance(5); // Cache miss lookup latency
        return nullptr;
    }
}

void SetAssociativeCache::put(int groupIndex, const GroupData& data, CycleCounter& cycleCounter) {
    size_t setIdx = getSetIndex(groupIndex);
    auto& set = sets[setIdx];

    if (set.entries.find(groupIndex) != set.entries.end()) {
        set.lruList.erase(set.entries[groupIndex].second);
    } else if (set.entries.size() >= waysPerSet) {
        int evictIndex = set.lruList.back();
        set.lruList.pop_back();
        set.entries.erase(evictIndex);
    }

    set.lruList.push_front(groupIndex);
    set.entries[groupIndex] = {data, set.lruList.begin()};

    cycleCounter.advance(1); // Cache write latency
}

void SetAssociativeCache::clear() {
    for (auto& set : sets) {
        set.entries.clear();
        set.lruList.clear();
    }
    hits = 0;
    misses = 0;
}

std::pair<int, int> SetAssociativeCache::printStats() const {
    std::cout << "Cache stats: Hits=" << hits << ", Misses=" << misses << "\n";
    return std::make_pair(hits, misses);
}

// =============================
// TwoLevelBuffer
// =============================

TwoLevelBuffer::TwoLevelBuffer(DRAMStorage& dram_, SetAssociativeCache& cache_)
    : dram(dram_), cache(cache_) {}

const GroupData& TwoLevelBuffer::get(int groupIndex) {
    bool wasHit;
    const GroupData* data = cache.get(groupIndex, wasHit, cycleCounter);
    if (wasHit) {
        return *data;
    } else {
        const GroupData& dramData = dram.load(groupIndex, cycleCounter);
        cache.put(groupIndex, dramData, cycleCounter);
        return dramData;
    }
}

void TwoLevelBuffer::put(int groupIndex, const GroupData& data) {
    dram.store(groupIndex, data, cycleCounter);
    cache.put(groupIndex, data, cycleCounter);
}

std::pair<int, int> TwoLevelBuffer::showCacheStats() const {
    return cache.printStats();
}

void TwoLevelBuffer::clear() {
    dram.clear();
    cache.clear();
    cycleCounter.reset();
}

size_t TwoLevelBuffer::getTotalCycles() const {
    return cycleCounter.get();
}

// =============================
// Scheduler
// =============================

Scheduler::Scheduler(TwoLevelBuffer& buffer_)
    : buffer(buffer_) {}

const GroupData& Scheduler::requestGroup(int groupIndex) {
    return buffer.get(groupIndex);
}

void Scheduler::storeGroup(int groupIndex, const GroupData& data) {
    buffer.put(groupIndex, data);
}

size_t Scheduler::getTotalCycles() const {
    return buffer.getTotalCycles();
}
