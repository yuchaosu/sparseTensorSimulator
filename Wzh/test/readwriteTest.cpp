// readwriteTest.cpp
// Parallel channel read/write streaming test using per-tick event stepping.
// Build: g++ -std=c++17 -O2 -o rw_test readwriteTest.cpp HBM.cpp

#include "HBM.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <random>
#include <iomanip>
#include <algorithm>

using AcceleratorHBM::HBMController;
using AcceleratorHBM::MemRequest;

// ------------------ Config ------------------
constexpr size_t NUM_WRITES = 2000;
constexpr size_t NUM_READS  = 2000;
constexpr size_t BURST_SIZE = 64;              // 64B bursts (HBMInterface bytesPerBurst)
constexpr uint32_t QUEUE_HIGH_WATERMARK = 100;
constexpr uint32_t QUEUE_LOW_WATERMARK  = 20;
constexpr uint32_t WRITE_BATCH_SIZE = 10;
constexpr uint32_t READ_BATCH_SIZE  = 10;

// ------------------ Helpers ------------------
uint64_t generatePattern(uint64_t addr, uint64_t offset) {
    return (addr << 32) | (offset & 0xFFFFFFFF) | 0xDEADBEEF00000000ULL;
}

static void printProgress(const std::string& phase, size_t done, size_t total) {
    const int barWidth = 40;
    double progress = total > 0 ? double(done) / total : 1.0;
    int pos = int(barWidth * progress);
    std::cout << "\r[" << phase << "] [";
    for (int i = 0; i < barWidth; ++i)
        std::cout << (i < pos ? "#" : (i == pos ? ">" : "-"));
    std::cout << "] " << std::setw(5) << std::fixed << std::setprecision(1)
              << (progress * 100.0) << "% (" << done << "/" << total << ")";
    std::cout.flush();
}

static void run_and_report(const std::string& phaseName,
                           HBMController& hbm,
                           uint64_t totalBytes,
                           uint64_t startTime,
                           uint64_t endTime)
{
    double elapsed = static_cast<double>(endTime - startTime);
    double bwGiB = (totalBytes * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n\n=== Phase: " << phaseName << " ===\n";
    std::cout << "Elapsed: " << elapsed << " ns\n";
    std::cout << "Transferred: " << (totalBytes / 1024.0 / 1024.0) << " MiB\n";
    std::cout << "Effective Bandwidth: " << bwGiB << " GiB/s\n";
    hbm.printStats(); // prints per-channel and req stats
}

// ===================== Test Harness =====================
class HBMReadWriteTest {
public:
    explicit HBMReadWriteTest(HBMController& controller)
        : hbm(controller)
    {
        // Pre-generate write requests (round 1)
        writeRequests.resize(NUM_WRITES);
        for (size_t i = 0; i < NUM_WRITES; i++) {
            writeRequests[i].addr = i * 0x1000;  // 4KiB stride
            writeRequests[i].issued = false;
            for (size_t j = 0; j < BURST_SIZE; j += 8) {
                uint64_t pat = generatePattern(writeRequests[i].addr, j);
                std::memcpy(&writeRequests[i].data[j], &pat, 8);
            }
        }
        // Pre-generate read requests (match round 1 addresses)
        readRequests.resize(NUM_READS);
        for (size_t i = 0; i < NUM_READS; i++) {
            readRequests[i].addr = i * 0x1000;
            readRequests[i].issued = false;
            std::memset(readRequests[i].data, 0, BURST_SIZE);
        }
    }

    void writePhase() {
        std::cout << "\n=== Phase 1: Write " << NUM_WRITES << " Requests ===\n";
        uint64_t sim = 1;
        writePhaseStartTime = sim;
        writesIssued = writesCompleted = 0;
        writePaused = false;

        auto statsBaseline = hbm.getStats();
        uint64_t writesBefore = statsBaseline.writeReqs;

        printProgress("Write", 0, NUM_WRITES);

        while (writesIssued < NUM_WRITES || hbm.hasOutstandingRequests()) {
            // Try to issue writes if not paused
            if (!writePaused && writesIssued < NUM_WRITES) {
                uint32_t qsize = hbm.writeQueueSize(); // total across channels
                if (qsize >= QUEUE_HIGH_WATERMARK) {
                    writePaused = true;
                } else {
                    for (uint32_t b = 0; b < WRITE_BATCH_SIZE && writesIssued < NUM_WRITES; ++b) {
                        auto& req = writeRequests[writesIssued];
                        MemRequest m{};
                        m.addr        = req.addr;
                        m.size        = BURST_SIZE;
                        m.isRead      = false;
                        m.isWrite     = true;
                        m.arrivalTime = sim;
                        m.data        = req.data;
                        if (!hbm.recvTimingReq(&m, sim)) { // enqueues bursts; controller selects channel
                            writePaused = true;
                            break;
                        }
                        req.issued = true;
                        writesIssued++;
                    }
                }
            } else if (writePaused && hbm.writeQueueSize() <= QUEUE_LOW_WATERMARK) {
                writePaused = false;
            }

            // Parallel step: advance all channels up to this tick
            hbm.processAllEventsUpTo(sim);
            sim++;

            // Progress + instantaneous BW every 1000ns
            if (sim % 1000 == 0) {
                writesCompleted = hbm.getStats().writeReqs - writesBefore;
                uint64_t elapsed = sim - writePhaseStartTime;
                double bw = (writesCompleted * BURST_SIZE * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
                printProgress("Write", writesCompleted, NUM_WRITES);
                std::cout << " [" << std::fixed << std::setprecision(2) << bw
                          << " GiB/s, WQ:" << hbm.writeQueueSize() << "]";
                std::cout.flush();
            }
        }

        writePhaseEndTime = sim;
        writesCompleted = hbm.getStats().writeReqs - writesBefore;
        uint64_t elapsed = writePhaseEndTime - writePhaseStartTime;
        double finalBw = (writesCompleted * BURST_SIZE * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
        printProgress("Write", writesCompleted, NUM_WRITES);
        std::cout << "  ✓ Done in " << elapsed << " ns"
                  << " [" << std::fixed << std::setprecision(2) << finalBw << " GiB/s]\n";

        run_and_report("Write Phase", hbm, writesCompleted * BURST_SIZE,
                       writePhaseStartTime, writePhaseEndTime);
    }

    void readWriteConcurrentPhase() {
        std::cout << "\n=== Phase 2: Concurrent Read " << NUM_READS
                  << " + Write " << NUM_WRITES << " Requests ===\n";

        uint64_t sim = writePhaseEndTime + 1;
        readPhaseStartTime = sim;
        readsIssued = writesIssued = readsCompleted = writesCompleted = 0;
        readPaused = writePaused = false;

        // Prepare write round 2 patterns (same addresses but new data)
        for (auto& req : writeRequests) {
            req.issued = false;
            for (size_t j = 0; j < BURST_SIZE; j += 8) {
                uint64_t pat = generatePattern(req.addr + 0x100000, j);
                std::memcpy(&req.data[j], &pat, 8);
            }
        }

        auto statsBaseline = hbm.getStats();
        uint64_t readsBefore  = statsBaseline.readReqs;
        uint64_t writesBefore = statsBaseline.writeReqs;

        printProgress("Concurrent", 0, NUM_READS + NUM_WRITES);

        while (readsIssued < NUM_READS || writesIssued < NUM_WRITES || hbm.hasOutstandingRequests()) {
            // Issue reads
            if (!readPaused && readsIssued < NUM_READS) {
                uint32_t rqsize = hbm.readQueueSize();
                if (rqsize >= QUEUE_HIGH_WATERMARK) {
                    readPaused = true;
                } else {
                    for (uint32_t b = 0; b < READ_BATCH_SIZE && readsIssued < NUM_READS; ++b) {
                        auto& r = readRequests[readsIssued];
                        MemRequest m{};
                        m.addr        = r.addr;
                        m.size        = BURST_SIZE;
                        m.isRead      = true;
                        m.isWrite     = false;
                        m.arrivalTime = sim;
                        m.data        = r.data; // read buffer
                        if (!hbm.recvTimingReq(&m, sim)) {
                            readPaused = true;
                            break;
                        }
                        r.issued = true;
                        readsIssued++;
                    }
                }
            } else if (readPaused && hbm.readQueueSize() <= QUEUE_LOW_WATERMARK) {
                readPaused = false;
            }

            // Issue writes
            if (!writePaused && writesIssued < NUM_WRITES) {
                uint32_t wqsize = hbm.writeQueueSize();
                if (wqsize >= QUEUE_HIGH_WATERMARK) {
                    writePaused = true;
                } else {
                    for (uint32_t b = 0; b < WRITE_BATCH_SIZE && writesIssued < NUM_WRITES; ++b) {
                        auto& w = writeRequests[writesIssued];
                        MemRequest m{};
                        m.addr        = w.addr;
                        m.size        = BURST_SIZE;
                        m.isRead      = false;
                        m.isWrite     = true;
                        m.arrivalTime = sim;
                        m.data        = w.data;
                        if (!hbm.recvTimingReq(&m, sim)) {
                            writePaused = true;
                            break;
                        }
                        w.issued = true;
                        writesIssued++;
                    }
                }
            } else if (writePaused && hbm.writeQueueSize() <= QUEUE_LOW_WATERMARK) {
                writePaused = false;
            }

            // Parallel step: advance all channels up to this tick
            hbm.processAllEventsUpTo(sim);
            sim++;

            // Periodic throughput display
            if (sim % 1000 == 0) {
                readsCompleted  = hbm.getStats().readReqs  - readsBefore;
                writesCompleted = hbm.getStats().writeReqs - writesBefore;
                uint64_t elapsed = sim - readPhaseStartTime;
                double readBw  = (readsCompleted  * BURST_SIZE * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
                double writeBw = (writesCompleted * BURST_SIZE * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
                double totalBw = readBw + writeBw;
                printProgress("Concurrent", readsCompleted + writesCompleted, NUM_READS + NUM_WRITES);
                std::cout << " [R:" << std::fixed << std::setprecision(2) << readBw
                          << " W:" << writeBw << " Tot:" << totalBw
                          << " GiB/s, RQ:" << hbm.readQueueSize()
                          << " WQ:" << hbm.writeQueueSize() << "]";
                std::cout.flush();
            }
        }

        readPhaseEndTime = sim;
        readsCompleted  = hbm.getStats().readReqs  - readsBefore;
        writesCompleted = hbm.getStats().writeReqs - writesBefore;
        uint64_t elapsed = readPhaseEndTime - readPhaseStartTime;

        double readBw  = (readsCompleted  * BURST_SIZE * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
        double writeBw = (writesCompleted * BURST_SIZE * 1e9) / (elapsed * 1024.0 * 1024.0 * 1024.0);
        double totalBw = readBw + writeBw;

        printProgress("Concurrent", readsCompleted + writesCompleted, NUM_READS + NUM_WRITES);
        std::cout << "  ✓ Done in " << elapsed << " ns"
                  << " [R:" << std::fixed << std::setprecision(2) << readBw
                  << " W:" << writeBw << " Total:" << totalBw << " GiB/s]\n";

        run_and_report("Concurrent Read+Write", hbm,
                       (readsCompleted + writesCompleted) * BURST_SIZE,
                       readPhaseStartTime, readPhaseEndTime);
    }

    bool verifyPhase() {
        std::cout << "\n=== Phase 3: Verify Data Integrity ===\n";
        verificationErrors = 0;
        size_t checked = 0;

        printProgress("Verify", 0, NUM_READS);

        for (size_t i = 0; i < NUM_READS; i++) {
            const auto& r = readRequests[i];

            // Expected pattern from Phase 1 writes
            uint8_t expected[BURST_SIZE];
            for (size_t j = 0; j < BURST_SIZE; j += 8) {
                uint64_t pat = generatePattern(r.addr, j);
                std::memcpy(&expected[j], &pat, 8);
            }
            if (std::memcmp(r.data, expected, BURST_SIZE) != 0) {
                verificationErrors++;
                if (verificationErrors <= 5) {
                    std::cout << "\n[ERROR] Mismatch at addr 0x" << std::hex << r.addr << std::dec;
                }
            }

            checked++;
            if (checked % 100 == 0) {
                printProgress("Verify", checked, NUM_READS);
            }
        }
        printProgress("Verify", NUM_READS, NUM_READS);
        if (verificationErrors == 0) {
            std::cout << "  ✓ All data verified correctly!\n";
            return true;
        } else {
            std::cout << "  ✗ " << verificationErrors << " verification errors!\n";
            return false;
        }
    }

private:
    struct Request {
        uint64_t addr;
        uint8_t  data[BURST_SIZE];
        bool     issued;
    };

    HBMController& hbm;

    std::vector<Request> writeRequests;
    std::vector<Request> readRequests;

    // State & stats
    bool writePaused = false, readPaused = false;
    size_t writesIssued = 0, readsIssued = 0;
    size_t writesCompleted = 0, readsCompleted = 0;
    size_t verificationErrors = 0;
    uint64_t writePhaseStartTime = 0, writePhaseEndTime = 0;
    uint64_t readPhaseStartTime  = 0, readPhaseEndTime  = 0;
};

int main() {
    std::cout << "========================================\n";
    std::cout << "  HBM Read-Write-Check Test (Parallel)\n";
    std::cout << "========================================\n";
    std::cout << "\nConfiguration:\n";
    std::cout << "  Write requests: " << NUM_WRITES << "\n";
    std::cout << "  Read requests:  " << NUM_READS  << "\n";
    std::cout << "  Burst size:     " << BURST_SIZE << " bytes\n";
    std::cout << "  Total data (counting R+W): "
              << ((NUM_WRITES + NUM_READS * 2) * BURST_SIZE / 1024 / 1024) << " MiB\n";

    // Initialize HBM (8 channels, 4 GiB)
    std::cout << "\nInitializing HBM (8 channels, 4 GiB)...\n";
    HBMController hbm(8);
    uint64_t memSize = 4ULL * 1024 * 1024 * 1024;
    hbm.init(memSize);
    hbm.startup(0);
    std::cout << "  ✓ HBM initialized\n";
    std::cout << "  Memory size: " << (memSize / 1024.0 / 1024.0 / 1024.0) << " GiB\n";

    // Run test
    HBMReadWriteTest test(hbm);
    test.writePhase();
    test.readWriteConcurrentPhase();
    bool verified = test.verifyPhase();

    std::cout << "\n========================================\n";
    if (verified) std::cout << "  ✓✓✓ TEST PASSED ✓✓✓\n";
    else          std::cout << "  ✗✗✗ TEST FAILED ✗✗✗\n";
    std::cout << "========================================\n";
    return verified ? 0 : 1;
}
