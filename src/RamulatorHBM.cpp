// Ramulator 2.1 in-loop DRAM adapter. Compiled as C++20 (Ramulator requires it)
// and linked against libramulator.so. See include/RamulatorHBM.h for the rationale.

#include "../include/RamulatorHBM.h"

#include <cstdlib>
#include <functional>
#include <stdexcept>

#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

namespace {
std::string resolve_config(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    if (const char* env = std::getenv("RAMULATOR_HBM_CONFIG")) return std::string(env);
#ifdef RAMULATOR_HBM_CONFIG
    return std::string(RAMULATOR_HBM_CONFIG);
#else
    return std::string("config/hbm4_sota.yaml");
#endif
}
}  // namespace

struct RamulatorHBM::Impl {
    Ramulator::IFrontEnd* frontend = nullptr;
    Ramulator::IMemorySystem* mem = nullptr;

    uint64_t outstanding = 0;       // requests issued but not yet completed
    uint64_t elapsed_cycles = 0;    // DRAM cycles ticked (only advanced while working)
    double tck_ns = 1.0;            // ns per DRAM cycle
    int tx_bytes = 32;              // bytes per device transaction

    uint64_t completed_reads = 0;
    long double read_latency_cycles = 0.0L;

    void build(const std::string& path) {
        Ramulator::ConfigNode cfg = Ramulator::Config::parse_config_file(path);
        frontend = Ramulator::Factory::create_frontend(cfg);
        mem = Ramulator::Factory::create_memory_system(cfg);
        if (!frontend || !mem) {
            throw std::runtime_error("RamulatorHBM: failed to build memory system from " + path);
        }
        frontend->connect_memory_system(mem);
        mem->connect_frontend(frontend);
        tck_ns = mem->get_tCK();
        tx_bytes = mem->get_tx_bytes();
        if (tx_bytes <= 0) tx_bytes = 32;
        if (tck_ns <= 0.0) tck_ns = 1.0;
    }
};

RamulatorHBM::RamulatorHBM() : RamulatorHBM(std::string()) {}

RamulatorHBM::RamulatorHBM(const std::string& config_path) : impl_(new Impl) {
    impl_->build(resolve_config(config_path));
}

RamulatorHBM::~RamulatorHBM() {
    // Match the gem5 wrapper's teardown order.
    delete impl_->frontend;
    delete impl_->mem;
}

void RamulatorHBM::tick() {
    impl_->mem->tick();
    ++impl_->elapsed_cycles;
}

bool RamulatorHBM::hasPendingRequests() const {
    return impl_->outstanding > 0;
}

uint64_t RamulatorHBM::getMaxChannelTime() const {
    return static_cast<uint64_t>(impl_->elapsed_cycles * impl_->tck_ns);
}

double RamulatorHBM::tCK() const { return impl_->tck_ns; }
int RamulatorHBM::txBytes() const { return impl_->tx_bytes; }

double RamulatorHBM::avgReadLatencyNs() const {
    if (impl_->completed_reads == 0) return 0.0;
    return static_cast<double>(impl_->read_latency_cycles / impl_->completed_reads) * impl_->tck_ns;
}

void RamulatorHBM::addRequest(uint64_t addr, bool isWrite, size_t size, int /*id*/) {
    const int tx = impl_->tx_bytes;
    size_t nchunks = (size + tx - 1) / static_cast<size_t>(tx);
    if (nchunks == 0) nchunks = 1;

    const int type = isWrite ? Ramulator::Request::Type::Write
                             : Ramulator::Request::Type::Read;
    const bool is_read = !isWrite;
    Impl* impl = impl_.get();

    for (size_t c = 0; c < nchunks; ++c) {
        const Ramulator::Addr_t a =
            static_cast<Ramulator::Addr_t>(addr + c * static_cast<uint64_t>(tx));

        auto cb = [impl, is_read](Ramulator::Request& req) {
            if (impl->outstanding > 0) --impl->outstanding;
            if (is_read) {
                ++impl->completed_reads;
                if (req.depart >= 0 && req.arrive >= 0 && req.depart >= req.arrive) {
                    impl->read_latency_cycles +=
                        static_cast<long double>(req.depart - req.arrive);
                }
            }
        };

        ++impl->outstanding;
        // Backpressure: if the DRAM request queue is full, advance the model and retry.
        while (!impl->frontend->receive_external_requests(type, a, 0, cb, tx)) {
            tick();
        }
    }
}
