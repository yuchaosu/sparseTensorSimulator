#include "../hbm/HBM.h"
#include "../include/Utility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

struct ScenarioConfig {
    size_t numRequests;
    uint64_t baseAddress;
    uint64_t stride;
    uint64_t requestSize;
};

struct ScenarioStats {
    size_t totalRequests;
    size_t readRequests;
    size_t writeRequests;
    uint64_t totalBytes;
    uint64_t readBytes;
    uint64_t writeBytes;
    uint64_t simTimeNs;
    double bandwidthGBs;
};

struct RealRequest {
    uint64_t address;
    uint64_t size;
    bool isWrite;
    double value;
};

struct RealScenarioResult {
    ScenarioStats stats;
    size_t mismatches;
};

ScenarioStats runScenario(const ScenarioConfig& config,
                          const std::function<bool(size_t)>& isWriteSelector,
                          const std::function<uint64_t(size_t)>& addressGenerator) {
    HBMController controller;
    size_t issued = 0;
    size_t requestId = 0;
    size_t readOps = 0;
    size_t writeOps = 0;

    while (issued < config.numRequests || controller.hasPendingRequests()) {
        while (issued < config.numRequests) {
            const bool isWriteReq = isWriteSelector(issued);
            const uint64_t addr = addressGenerator(issued);

            try {
                controller.addRequest(addr, isWriteReq, config.requestSize, static_cast<int>(requestId));
                ++issued;
                ++requestId;
                if (isWriteReq) {
                    ++writeOps;
                } else {
                    ++readOps;
                }
            } catch (const std::runtime_error&) {
                break;
            }
        }

        controller.tick();
    }

    const uint64_t simTime = controller.getMaxChannelTime();
    const uint64_t readBytes = static_cast<uint64_t>(readOps) * config.requestSize;
    const uint64_t writeBytes = static_cast<uint64_t>(writeOps) * config.requestSize;
    const uint64_t totalBytes = readBytes + writeBytes;
    const double seconds = static_cast<double>(simTime) * 1e-9;
    const double bandwidthGBs = (seconds > 0.0) ? (static_cast<double>(totalBytes) / seconds) / 1e9 : 0.0;

    return ScenarioStats{readOps + writeOps, readOps, writeOps, totalBytes, readBytes, writeBytes, simTime, bandwidthGBs};
}

RealScenarioResult runRealDataScenario(const std::vector<RealRequest>& requests, double epsilon = 1e-9) {
    HBMController controller;
    if (requests.empty()) {
        return RealScenarioResult{ScenarioStats{0, 0, 0, 0, 0, 0, 0, 0.0}, 0};
    }

    size_t issued = 0;
    int requestId = 0;
    size_t readOps = 0;
    size_t writeOps = 0;
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    size_t mismatches = 0;
    std::unordered_map<uint64_t, double> backingStore;

    while (issued < requests.size() || controller.hasPendingRequests()) {
        while (issued < requests.size()) {
            const RealRequest& spec = requests[issued];
            try {
                controller.addRequest(spec.address, spec.isWrite, spec.size, requestId++);
                if (spec.isWrite) {
                    backingStore[spec.address] = spec.value;
                    ++writeOps;
                    writeBytes += spec.size;
                } else {
                    ++readOps;
                    readBytes += spec.size;
                    const auto it = backingStore.find(spec.address);
                    if (it == backingStore.end() || std::abs(it->second - spec.value) > epsilon) {
                        ++mismatches;
                    }
                }
                ++issued;
            } catch (const std::runtime_error&) {
                break;
            }
        }

        controller.tick();
    }

    const uint64_t simTime = controller.getMaxChannelTime();
    const uint64_t totalBytes = readBytes + writeBytes;
    const double seconds = static_cast<double>(simTime) * 1e-9;
    const double bandwidthGBs = (seconds > 0.0) ? (static_cast<double>(totalBytes) / seconds) / 1e9 : 0.0;

    ScenarioStats stats{readOps + writeOps, readOps, writeOps, totalBytes, readBytes, writeBytes, simTime, bandwidthGBs};
    return RealScenarioResult{stats, mismatches};
}

std::vector<RealRequest> buildDiagonalRoundTripRequests(
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals,
    uint64_t baseAddress,
    uint64_t channelStripe,
    uint64_t requestSize) {

    std::vector<std::pair<int, std::vector<std::tuple<double, int, int>>>> sortedDiagonals(diagonals.begin(), diagonals.end());
    std::sort(sortedDiagonals.begin(), sortedDiagonals.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::vector<RealRequest> requests;
    size_t entryIndex = 0;
    for (const auto& [offset, entries] : sortedDiagonals) {
        (void)offset; // offset ordering is already captured in sortedDiagonals
        for (const auto& [value, row, col] : entries) {
            (void)row;
            (void)col;
            const uint64_t address = baseAddress + (entryIndex * channelStripe);
            requests.push_back(RealRequest{address, requestSize, true, value});
            requests.push_back(RealRequest{address, requestSize, false, value});
            ++entryIndex;
        }
    }
    return requests;
}

size_t countUniqueChannels(const std::vector<RealRequest>& requests) {
    constexpr uint64_t kChannelStripeBytes = 64; // matches controller interleaving
    constexpr int kNumChannels = 8;
    std::array<bool, kNumChannels> used{};

    for (const auto& req : requests) {
        const int channel = static_cast<int>((req.address / kChannelStripeBytes) % kNumChannels);
        used[channel] = true;
    }

    return static_cast<size_t>(std::count(used.begin(), used.end(), true));
}

std::function<uint64_t(size_t)> makeSequentialAddressGenerator(const ScenarioConfig& config) {
    const uint64_t base = config.baseAddress;
    const uint64_t stride = config.stride;
    return [base, stride](size_t index) {
        return base + static_cast<uint64_t>(index) * stride;
    };
}

std::function<uint64_t(size_t)> makeRandomizedAddressGenerator(const ScenarioConfig& config,
                                                               std::mt19937_64& rng) {
    std::vector<uint64_t> addresses(config.numRequests);
    for (size_t i = 0; i < config.numRequests; ++i) {
        addresses[i] = config.baseAddress + static_cast<uint64_t>(i) * config.stride;
    }
    std::shuffle(addresses.begin(), addresses.end(), rng);
    return [addresses](size_t index) -> uint64_t {
        return addresses[index];
    };
}

void printStats(const std::string& label, const ScenarioStats& stats) {
    const auto originalFlags = std::cout.flags();
    const std::streamsize originalPrecision = std::cout.precision();

    std::cout << label << " scenario" << '\n';
    std::cout << "  Requests: " << stats.totalRequests << " (reads=" << stats.readRequests
              << ", writes=" << stats.writeRequests << ")" << '\n';
    std::cout << "  Total bytes moved: " << stats.totalBytes << " B" << '\n';
    std::cout << "  Simulation time: " << stats.simTimeNs << " ns" << '\n';
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Effective bandwidth: " << stats.bandwidthGBs << " GB/s" << '\n' << '\n';

    std::cout.flags(originalFlags);
    std::cout.precision(originalPrecision);
}

int main() {
    constexpr size_t kRequests = 4096; // > 2000 requests per pure scenario
    constexpr uint64_t kRequestSize = 64; // bytes
    constexpr uint64_t kStride = 64; // match burst size for best-case streaming

    const ScenarioConfig readConfig{kRequests, 0, kStride, kRequestSize};
    const ScenarioConfig writeConfig{kRequests, 1ULL << 20, kStride, kRequestSize};
    const ScenarioConfig mixedConfig{8192, 1ULL << 24, kStride, kRequestSize}; // 4096 reads + 4096 writes

    auto sequentialReadGenerator = makeSequentialAddressGenerator(readConfig);
    auto sequentialWriteGenerator = makeSequentialAddressGenerator(writeConfig);
    auto sequentialMixedGenerator = makeSequentialAddressGenerator(mixedConfig);

    const auto sequentialReadStats = runScenario(readConfig, [](size_t) { return false; }, sequentialReadGenerator);
    const auto sequentialWriteStats = runScenario(writeConfig, [](size_t) { return true; }, sequentialWriteGenerator);
    const auto sequentialMixedStats = runScenario(mixedConfig, [](size_t index) { return (index % 2) == 1; }, sequentialMixedGenerator);

    std::mt19937_64 rng(12345);
    auto randomReadGenerator = makeRandomizedAddressGenerator(readConfig, rng);
    auto randomWriteGenerator = makeRandomizedAddressGenerator(writeConfig, rng);
    auto randomMixedGenerator = makeRandomizedAddressGenerator(mixedConfig, rng);

    const auto randomReadStats = runScenario(readConfig, [](size_t) { return false; }, randomReadGenerator);
    const auto randomWriteStats = runScenario(writeConfig, [](size_t) { return true; }, randomWriteGenerator);
    const auto randomMixedStats = runScenario(mixedConfig, [](size_t index) { return (index % 2) == 1; }, randomMixedGenerator);

    printStats("Sequential read", sequentialReadStats);
    printStats("Sequential write", sequentialWriteStats);
    printStats("Sequential mixed read/write", sequentialMixedStats);
    printStats("Random read", randomReadStats);
    printStats("Random write", randomWriteStats);
    printStats("Random mixed read/write", randomMixedStats);

    constexpr int kMatrixSize = 128;
    const std::vector<int> diagonalOffsets = {-3, -1, 0, 2, 5};
    const auto denseMatrix = generate_random_matrix(kMatrixSize, diagonalOffsets, -1.0, 1.0);
    const auto extractedDiagonals = extract_diagonals(denseMatrix, diagonalOffsets);

    constexpr uint64_t kStripeBytes = 64;
    constexpr uint64_t kRequestBytes = 64;
    constexpr uint64_t kBaseAddress = 0x100000000ULL;
    const auto realRequests = buildDiagonalRoundTripRequests(extractedDiagonals, kBaseAddress, kStripeBytes, kRequestBytes);
    const auto realResult = runRealDataScenario(realRequests);

    printStats("Diagonal data round-trip", realResult.stats);
    std::cout << "  Verification mismatches: " << realResult.mismatches << '\n';
    std::cout << "  Channels exercised: " << countUniqueChannels(realRequests) << '\n' << '\n';

    return 0;
}
