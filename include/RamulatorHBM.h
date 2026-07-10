#ifndef RAMULATOR_HBM_H
#define RAMULATOR_HBM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// C++17-facing adapter around Ramulator 2.1 (the implementation TU is compiled
// as C++20 and links libramulator.so). It exposes exactly the subset of the
// old homegrown HBMController that the HBM drivers use, so it is a drop-in
// replacement — the drivers keep their allocation/striping logic and only the
// DRAM timing engine changes to a peer-reviewed model (SOTA HBM4 by default).
//
// All Ramulator headers are hidden behind a PIMPL so the rest of the (C++17)
// simulator never sees C++20 Ramulator types.
class RamulatorHBM {
public:
    // Uses the config at $RAMULATOR_HBM_CONFIG, else the compiled-in default
    // (config/hbm4_sota.yaml). See config/hbm4_sota.yaml for the SOTA HBM4 stack.
    RamulatorHBM();
    explicit RamulatorHBM(const std::string& config_path);
    ~RamulatorHBM();

    RamulatorHBM(const RamulatorHBM&) = delete;
    RamulatorHBM& operator=(const RamulatorHBM&) = delete;

    // Enqueue a transfer of `size` bytes at byte address `addr`. The transfer is
    // split internally into device-transaction-sized (tx) Ramulator requests so
    // byte and bandwidth accounting match the modeled DRAM. `id` is accepted for
    // API compatibility and ignored. Backpressure is handled internally: if the
    // DRAM queue is full the call ticks the model until the request is accepted.
    void addRequest(uint64_t addr, bool isWrite, size_t size, int id);

    void tick();                        // advance the DRAM model one cycle
    bool hasPendingRequests() const;    // true while requests are in flight

    // Accumulated DRAM active time, in nanoseconds (monotonic). Named to match
    // the old HBMController so callers that stored this as "cycles"/ns are kept.
    uint64_t getMaxChannelTime() const;

    double tCK() const;                 // ns per DRAM cycle
    int txBytes() const;                // bytes per device transaction
    double avgReadLatencyNs() const;    // mean read latency over completed reads

    // Trace hooks kept only for source compatibility with HBMController (no-ops:
    // Ramulator has its own stat collection).
    void enableTrace(const std::string&) {}
    void disableTrace() {}
    void enableTraceAggregate(const std::string&) {}
    void disableTraceAggregate() {}

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // RAMULATOR_HBM_H
