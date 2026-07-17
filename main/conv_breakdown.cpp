// conv_breakdown.cpp -- STANDALONE cycle-breakdown driver for the offset-space DIA
// convolution dataflow. It exists to answer one question for the paper: in the
// overlapped (pipelined) convOnGrid dataflow, how many cycles are HIDDEN by the
// scatter->MAC->gather overlap, vs EXPOSED in the makespan?
//
// It does NOT touch main/diamond.cpp. It carries a self-contained COPY of the same
// cycle model (loadDiaMatrix + convOnGrid), reusing the REAL PE class so the MAC
// makespan is genuine (not re-derived), and adds the per-part breakdown emission:
//   compute_mac  = busiest-PE MAC makespan (the lockstep loop result)
//   gather_thr   = TOTAL NoC/reduce cycles the flexible-NoC gather stage costs
//   filldrain    = one-time pipeline ramp (transport diameter + reduction-tree depth)
// From these three primitives, per iteration:
//   exposed = max(0, gather_thr - compute_mac)     -> cycles that stick out past compute
//   hidden  = min(compute_mac, gather_thr)         -> cycles that OVERLAP compute (the point)
//   makespan= compute_mac + exposed + filldrain    (== convOnGrid's cyc; asserted)
//   serial  = compute_mac + gather_thr + filldrain (hypothetical no-overlap)
//   overlap_savings = serial - makespan = hidden
//
// This is CYCLE DOMAIN ONLY -- no DRAM/HBM (the user's "the cycle is enough"), so it
// links against PE alone, not Ramulator.
//
// Build:  make conv_breakdown        Run:  ./outputs/conv_breakdown -file=... [opts]

#include "../include/PE.h"
#include "../include/Utility.h"   // DataPackage
#include "../include/Fifo.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using ConvDiag = std::unordered_map<int, std::vector<std::tuple<double,int,int>>>;

// ---- COPY of diamond.cpp:loadDiaMatrix (DIA file "N <n> D <d>" then "<off>: v v ...") ----
static long loadDiaMatrix(const std::string& filename,
                          std::vector<int>& offsets, ConvDiag& diag) {
    std::ifstream in(filename);
    if (!in) return -1;
    std::string first;
    if (!std::getline(in, first)) return -1;
    long n = 0, D = 0;
    {
        std::istringstream iss(first);
        std::string tok;
        if (!(iss >> tok) || tok != "N") return -1;
        if (!(iss >> n)) return -1;
        if (!(iss >> tok) || tok != "D") return -1;
        iss >> D;
    }
    if (n <= 0) return -1;
    offsets.clear();
    diag.clear();
    std::string line;
    while (std::getline(in, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        int off = std::stoi(line.substr(0, colon));
        const long len = n - std::labs(static_cast<long>(off));
        if (len <= 0) continue;
        std::vector<std::tuple<double,int,int>> entries;
        const char* p = line.c_str() + colon + 1;
        char* end = nullptr;
        for (long k = 0; k < len; ++k) {
            double v = std::strtod(p, &end);
            if (end == p) break;
            p = end;
            if (v != 0.0) {
                int row, col;
                if (off >= 0) { row = static_cast<int>(k);       col = static_cast<int>(k + off); }
                else          { row = static_cast<int>(k - off); col = static_cast<int>(k); }
                entries.emplace_back(v, row, col);
            }
        }
        if (!entries.empty()) {
            offsets.push_back(off);
            diag[off] = std::move(entries);
        }
    }
    std::sort(offsets.begin(), offsets.end());
    return n;
}

// ---- COPY of diamond.cpp:convOnGrid, with the per-part breakdown emission added ----
// Returns the product ConvDiag C (so powers chain across iterations). The three
// out-params compute_mac_out / gather_thr_out / filldrain_out expose the makespan's
// disjoint parts; makespan (cycles_out) reconciles to compute_mac + exposed + filldrain.
static ConvDiag convOnGrid(const ConvDiag& A, const ConvDiag& B, int n, int S,
                           bool zeroskip, bool cbalance, long& cycles_out,
                           long& compute_mac_out, long& gather_thr_out, long& filldrain_out,
                           int reduce_lanes = 0) {
    static std::ofstream devnull;
    const int P = std::max(1, S * S);
    auto toVec = [&](const ConvDiag& M) {
        std::unordered_map<int, std::vector<double>> v;
        for (const auto& [o, e] : M) { auto& a = v[o]; a.assign(n, 0.0); for (const auto& [val, r, c] : e) a[r] = val; }
        return v;
    };
    auto Av = toVec(A), Bv = toVec(B);

    struct PRef { int dA, dB, k; };
    std::vector<PRef> flat;
    for (const auto& [dA, va] : Av) {
        for (const auto& [dB, vb] : Bv) {
            const int dC = dA + dB;
            const int lo = std::max(0, std::max(-dA, -dC));
            const int hi = std::min(n, std::min(n - dA, n - dC));
            if (hi <= lo) continue;
            for (int k = lo; k < hi; ++k) {
                if (zeroskip && va[k] * vb[k + dA] == 0.0) continue;
                flat.push_back({dA, dB, k});
            }
        }
    }
    const long long total = (long long)flat.size();

    // Assign products to PEs: base (dC mod P, one PE owns each output diagonal) vs
    // cbalance (equal contiguous chunks over all P PEs -> breaks dC-locality, pays gather).
    std::vector<std::vector<long long>> peq(P);
    if (cbalance) {
        const long long chunk = (total + P - 1) / P;
        for (long long i = 0; i < total; ++i)
            peq[std::min<long long>(P - 1, chunk ? i / chunk : 0)].push_back(i);
    } else {
        for (long long i = 0; i < total; ++i) {
            int dC = flat[i].dA + flat[i].dB;
            peq[((dC % P) + P) % P].push_back(i);
        }
    }

    // Cross-PE gather load: a partial not on its home PE (dC mod P) must be routed+reduced.
    auto home = [P](int dC){ return ((dC % P) + P) % P; };
    long long crossPE = 0;
    for (int p = 0; p < P; ++p)
        for (long long idx : peq[p]) {
            const int dC = flat[idx].dA + flat[idx].dB;
            if (home(dC) != p) ++crossPE;
        }

    // Cycle-accurate run on the REAL PE: makespan = busiest PE's MAC queue.
    std::unordered_map<int, std::unordered_map<int, double>> acc;
    auto drain = [&](PE& pe) {
        while (!pe.PsumOut.isEmpty()) {
            DataPackage p = pe.PsumOut.front(); pe.PsumOut.pop();
            acc[p.index2 - p.index1][p.index1] += p.value;
        }
    };
    std::vector<std::unique_ptr<PE>> pes;
    for (int p = 0; p < P; ++p) {
        if (peq[p].empty()) continue;
        auto pe = std::make_unique<PE>(0, 0, devnull, peq[p].size() + 16);
        pe->setLastRow(true); pe->setLastCol(true);
        for (long long idx : peq[p]) {
            const PRef& pr = flat[idx];
            double a = Av[pr.dA][pr.k], b = Bv[pr.dB][pr.k + pr.dA];
            pe->receivedA.push(DataPackage(a, pr.k, pr.k + pr.dA));
            pe->receivedB.push(DataPackage(b, pr.k + pr.dA, pr.k + pr.dA + pr.dB));
        }
        pes.push_back(std::move(pe));
    }
    long cyc = 0; bool any = true;
    while (any) {
        any = false;
        for (auto& pe : pes) {
            if (!pe->receivedA.isEmpty()) { pe->cycle(static_cast<uint64_t>(cyc)); any = true; }
            drain(*pe);
        }
        if (any) ++cyc;
    }
    for (auto& pe : pes) drain(*pe);

    // ---- Per-part breakdown (see file header). compute_mac is the pre-gather makespan;
    // the gather stage OVERLAPS compute and only its exposed remainder + fill/drain add cycles.
    const long compute_mac = cyc;
    long gather_thr = 0, filldrain = 0;
    const long kReduceLanes = reduce_lanes > 0 ? std::min(reduce_lanes, P) : P;   // adds/cycle
    if (crossPE > 0) {
        gather_thr = static_cast<long>((crossPE + kReduceLanes - 1) / kReduceLanes);
        const long netDiameter = 2 * (S - 1);
        long treeDepth = 0; for (int m = P; m > 1; m = (m + 1) / 2) ++treeDepth;   // ceil(log2 P)
        filldrain = netDiameter + treeDepth;
        cyc = std::max(cyc, gather_thr) + netDiameter + treeDepth;   // == compute + exposed + filldrain
    }
    const long exposed = std::max(0L, gather_thr - compute_mac);
    if (compute_mac + exposed + filldrain != cyc) {
        std::cerr << "FATAL breakdown identity violated: " << compute_mac << "+" << exposed
                  << "+" << filldrain << " != " << cyc << "\n";
        std::abort();
    }
    compute_mac_out = compute_mac; gather_thr_out = gather_thr; filldrain_out = filldrain;
    cycles_out = cyc;

    ConvDiag C;
    for (auto& [dC, m] : acc) {
        std::vector<std::tuple<double,int,int>> ents;
        for (auto& [row, val] : m) if (val != 0.0) ents.emplace_back(val, row, row + dC);
        std::sort(ents.begin(), ents.end(), [](auto& x, auto& y){ return std::get<1>(x) < std::get<1>(y); });
        if (!ents.empty()) C[dC] = std::move(ents);
    }
    return C;
}

// ---- tiny "-key=val" arg parser ----
static std::unordered_map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::unordered_map<std::string, std::string> a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("-", 0) != 0) continue;
        s = s.substr(1);
        auto eq = s.find('=');
        if (eq == std::string::npos) a[s] = "1";
        else a[s.substr(0, eq)] = s.substr(eq + 1);
    }
    return a;
}

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    if (!args.count("file")) {
        std::cerr <<
          "usage: conv_breakdown -file=PATH [-row=128] [-col=128] [-iter=K]\n"
          "                      [-zeroskip=1] [-cbalance=1] [-reduce_lanes=0]\n"
          "                      [-tag=LABEL] [-csv=OUT.csv]\n"
          "  reduce_lanes<=0 -> well-provisioned P-wide network (default; gather fully hidden).\n"
          "  Narrow it (e.g. P/4) to EXPOSE part of the gather stage.\n"
          "  Emits one summary row (summed over the K powers). Appends a header if OUT is new.\n";
        return 1;
    }
    const std::string file = args["file"];
    const int S = args.count("row") ? std::stoi(args["row"]) : (args.count("col") ? std::stoi(args["col"]) : 128);
    const bool zeroskip = args.count("zeroskip") ? std::stoi(args["zeroskip"]) != 0 : true;
    const bool cbalance = args.count("cbalance") ? std::stoi(args["cbalance"]) != 0 : true;
    const int reduce_lanes = args.count("reduce_lanes") ? std::stoi(args["reduce_lanes"]) : 0;
    const std::string tag = args.count("tag") ? args["tag"] : "";
    const std::string csv = args.count("csv") ? args["csv"] : "";

    std::vector<int> offsets;
    ConvDiag current, H;
    long n = loadDiaMatrix(file, offsets, current);
    if (n <= 0 || offsets.empty()) { std::cerr << "bad/empty DIA file: " << file << "\n"; return 2; }
    H = current;   // B = H, reused across powers
    // K default: trailing _<K>.txt if present, else 1.
    int K = 1;
    if (args.count("iter")) K = std::stoi(args["iter"]);
    else {
        auto slash = file.find_last_of('/');
        std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
        std::istringstream tokss(base);
        std::string t; std::vector<std::string> parts;
        // split on '_' and '.'
        std::string cur; for (char c : base) { if (c == '_' || c == '.') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur += c; }
        if (parts.size() >= 2) { try { K = std::stoi(parts[parts.size() - 2]); } catch (...) { K = 1; } }
        if (K <= 0) K = 1;
    }

    long tot_makespan = 0, tot_compute = 0, tot_gather = 0, tot_filldrain = 0, tot_exposed = 0, tot_hidden = 0;
    for (int it = 0; it < K; ++it) {
        long ms = 0, cm = 0, gt = 0, fd = 0;
        ConvDiag C = convOnGrid(current, H, static_cast<int>(n), S, zeroskip, cbalance, ms, cm, gt, fd, reduce_lanes);
        const long exposed = std::max(0L, gt - cm);
        const long hidden  = std::min(cm, gt);
        tot_makespan += ms; tot_compute += cm; tot_gather += gt; tot_filldrain += fd;
        tot_exposed += exposed; tot_hidden += hidden;
        current = std::move(C);
    }
    const long serial = tot_compute + tot_gather + tot_filldrain;   // hypothetical no-overlap
    const long overlap_savings = tot_hidden;                        // serial - makespan
    const double hidden_pct = tot_gather > 0 ? 100.0 * (double)tot_hidden / (double)tot_gather : 0.0;

    std::cout << "[breakdown] " << (tag.empty() ? file : tag)
              << " n=" << n << " S=" << S << " K=" << K
              << " zeroskip=" << zeroskip << " cbalance=" << cbalance << " reduce_lanes=" << reduce_lanes << "\n"
              << "  compute_mac=" << tot_compute << "  gather_thr=" << tot_gather
              << " (hidden=" << tot_hidden << " [" << hidden_pct << "% of gather], exposed=" << tot_exposed << ")"
              << "  filldrain=" << tot_filldrain << "\n"
              << "  makespan=" << tot_makespan << "  serial(no-overlap)=" << serial
              << "  overlap_savings=" << overlap_savings << "\n";

    if (!csv.empty()) {
        std::ifstream probe(csv);
        const bool need_hdr = !probe.good() || probe.peek() == std::ifstream::traits_type::eof();
        probe.close();
        std::ofstream out(csv, std::ios::app);
        if (need_hdr)
            out << "tag,file,n,S,K,zeroskip,cbalance,reduce_lanes,"
                   "compute_mac,gather_thr,filldrain,gather_exposed,gather_hidden,"
                   "makespan,serial,overlap_savings\n";
        out << (tag.empty() ? file : tag) << ',' << file << ',' << n << ',' << S << ',' << K << ','
            << (zeroskip ? 1 : 0) << ',' << (cbalance ? 1 : 0) << ',' << reduce_lanes << ','
            << tot_compute << ',' << tot_gather << ',' << tot_filldrain << ',' << tot_exposed << ','
            << tot_hidden << ',' << tot_makespan << ',' << serial << ',' << overlap_savings << '\n';
    }
    return 0;
}
