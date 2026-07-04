// accel_compare.cpp — high-level driver that runs the SAME workload (Taylor powers of a
// Hamiltonian H) on three accelerators (DIAMOND / TPU / Trapezoid-MS / Trapezoid-HS) under
// shared fairness constants (equal PEs, one HBM4 config, one on-chip buffer budget), VERIFIES
// each against the dense reference, and prints a component breakdown comparison table.
//
// Usage: accel_compare -file=synth_6_2.txt -qubit=6 -iter=2 -row=8   (row = S; PEs = S*S)
#include <iostream>
#include <iomanip>
#include <map>
#include <string>
#include <vector>
#include "../include/Accelerators.h"

using namespace accel;

int main(int argc, char** argv) {
    std::map<std::string,std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]); size_t eq = a.find('=');
        if (a.rfind("-",0)==0 && eq!=std::string::npos) args[a.substr(1,eq-1)] = a.substr(eq+1);
    }
    std::string folder = args.count("folder") ? args["folder"] : "/mnt/beegfs/ysu34/hamlib/dia_oom/";
    std::string file   = args.count("file")   ? args["file"]   : "synth_6_2.txt";
    int iter = args.count("iter") ? std::stoi(args["iter"]) : 2;
    int S    = args.count("row")  ? std::stoi(args["row"])  : 8;      // array side; PEs = S*S
    std::string only = args.count("only") ? args["only"] : "";        // run one accelerator (name substr)

    ConvDiag H;
    long n = loadDia(folder + file, H);
    if (n <= 0) { std::cerr << "failed to load DIA: " << folder + file << "\n"; return 1; }
    std::cout << "Workload: " << file << "  n=" << n << "  iters=" << iter
              << "  |  Shared fairness: PEs=" << (S*S) << " (" << S << "x" << S << "), "
              << "HBM4=Ramulator2 (config/hbm4_sota.yaml), on-chip buffer=" << (kScratchpadBytes/1024) << " KiB\n";
    // NB: no dense verification here — kernel correctness is established separately at small n
    // (symmetric + asymmetric fixtures). The dense O(n^2) reference would cap the sweep at
    // q~13, so it is intentionally removed to let the real sparse simulator reach high q.

    DiamondAccel diamond; TpuAccel tpu; TrapMSAccel trapms; TrapHSAccel traphs;
    std::vector<Accelerator*> all = { &diamond, &tpu, &trapms, &traphs };
    std::vector<Accelerator*> accels;
    for (auto* a : all) if (only.empty() || std::string(a->name()).find(only) != std::string::npos) accels.push_back(a);

    struct Row { std::string name; Breakdown bd; bool ran; std::string note; };
    std::vector<Row> rows;

    for (auto* acc : accels) {
        try {
            PE::resetActivity(); DiagonalReduction::resetAccumulation();
            AccelHBM mem;               // fresh Ramulator2 HBM4 state, same config for every accel
            ConvDiag C = H;             // C_0 = H^1
            long long tot_cyc=0, ar=0, aa=0, ab=0, hbm=0, peak=0, spill=0;
            for (int k = 0; k < iter; ++k)
                C = acc->matmul(C, H, (int)n, S, tot_cyc, ar, aa, ab, hbm, peak, spill, mem);
            const double dram_ns = static_cast<double>(mem.dramNs());   // real HBM4 active time (Ramulator2)
            Breakdown bd = computeBreakdown(S, S, tot_cyc, hbm, peak, spill, dram_ns, ar, aa, ab);
            rows.push_back({ acc->name(), bd, true, "" });
        } catch (const std::bad_alloc&) {
            std::cerr << "[OOM] " << acc->name() << " ran out of memory at n=" << n << "\n";
            rows.push_back({ acc->name(), Breakdown{}, false, "OOM" });
        }
    }

    // ---- comparison table ----
    auto hr = [](){ std::cout << std::string(118,'-') << "\n"; };
    std::cout << std::left << std::setw(30) << "accelerator"
              << std::right << std::setw(8) << "status" << std::setw(14) << "cycles"
              << std::setw(9) << "PE-util" << std::setw(8) << "MAC%" << std::setw(8) << "MEM%"
              << std::setw(9) << "ROUTER%" << std::setw(9) << "BUFFER%" << std::setw(8) << "ACCUM%"
              << std::setw(11) << "buf-peak" << "\n";
    hr();
    std::cout << std::fixed << std::setprecision(2);
    for (auto& r : rows) {
        if (!r.ran) { std::cout << std::left << std::setw(30) << r.name << std::right << std::setw(8) << "OOM" << "\n"; continue; }
        std::cout << std::left << std::setw(30) << r.name
                  << std::right << std::setw(8) << "OK"
                  << std::setw(14) << r.bd.real_cycles
                  << std::setw(8) << r.bd.pe_util << "%"
                  << std::setw(8) << r.bd.mac_pct << std::setw(8) << r.bd.mem_pct
                  << std::setw(9) << r.bd.router_pct << std::setw(9) << r.bd.buffer_pct
                  << std::setw(8) << r.bd.accum_pct
                  << std::setw(9) << (r.bd.buffer_peak/1024) << "K" << "\n";
    }
    hr();
    std::cout << "cycles = pipelined real cycles (overlapped stages + per-tile fill/drain); "
              << "energy% normalized (MAC=1,BUF=1,ROUTER=2,ACCUM=1,MEM=200).\n";
    std::cout << "buf-peak = peak on-chip working set (budget " << (kScratchpadBytes/1024) << " KiB; spill to HBM if exceeded).\n";
    // detail lines
    std::cout << "\nper-accelerator counts (measured on real PE mesh; Trapezoid-HS routing/accum are computed addends):\n";
    for (auto& r : rows)
        std::cout << "  " << std::left << std::setw(30) << r.name
                  << "compute_cyc=" << r.bd.compute_cycles << " mac=" << r.bd.mac
                  << " cmp=" << r.bd.compare << " router=" << r.bd.router
                  << " buf=" << r.bd.buf << " accum=" << r.bd.accum
                  << " hbm_ns=" << (long long)r.bd.dram_ns << " (exposed=" << (long long)r.bd.exposed_ns << ")"
                  << " bottleneck=" << r.bd.bottleneck << "\n";
    return 0;
}
