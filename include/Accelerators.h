// Accelerators.h — three matrix-multiply accelerators (DIAMOND, TPU, Trapezoid) behind a
// common interface, all running TRUE cycle-accurate hardware on the SAME shared PE mesh
// primitives (PE/Grid/Connection/DiagonalReduction, linked from src/) under one shared set
// of fairness constants: same PE count, same HBM4 config, same on-chip buffer budget.
//
// This reuses the exact merge-join mesh logic verified in main/HBMHamiltonian.cpp; it lives
// in namespace `accel` so it never clashes with that file's globals (HBMHamiltonian keeps
// building unchanged). The heavy hardware (PE/Grid/...) IS literally the same code both use.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "PE.h"
#include "Grid.h"
#include "Connection.h"
#include "DiagonalReduction.h"
#include "RamulatorHBM.h"
#include "Utility.h"

namespace accel {

// ================= SHARED FAIRNESS CONSTANTS (identical for every accelerator) =========
// (1) PE count is grid_row*grid_col, passed uniformly by the driver.
// (2) HBM4: the SAME Ramulator 2.1 SOTA-HBM4 model (config/hbm4_sota.yaml) drives every
//     accelerator's DRAM timing in-loop (per-burst), not a bandwidth constant. See AccelHBM.
// (3) On-chip buffer: one fixed byte budget (the 2 MB edge scratchpad), same for all.
constexpr long long kScratchpadBytes    = 2LL * 1024 * 1024;

using ConvDiag = std::unordered_map<int, std::vector<std::tuple<double,int,int>>>;
using DenseMat = std::vector<std::vector<double>>;

// Per-burst SOTA-HBM4 model via Ramulator 2.1 -- the SAME engine HBMHamiltonian uses
// (issueTransfers -> addRequest -> tick/drain). One instance per accelerator (fresh DRAM
// state), same compiled-in config for all four, so the memory timing is genuinely modeled
// and identical across accelerators. transfer(bytes,isWrite) issues 64 B bursts through
// Ramulator and drains; dramNs() is the accumulated DRAM active time (ns).
class AccelHBM {
public:
    // Charge `bytes` of DRAM traffic as 64 B bursts through the real Ramulator model.
    void transfer(long long bytes, bool isWrite) {
        if (bytes <= 0) return;
        long long remaining = bytes;
        while (remaining > 0) {
            const size_t chunk = static_cast<size_t>(std::min<long long>(kBurstBytes, remaining));
            controller_.addRequest(addr_, isWrite, chunk, id_++);  // backpressure ticked internally
            addr_ += kBurstBytes;
            remaining -= static_cast<long long>(chunk);
        }
        if (isWrite) bytes_written_ += bytes; else bytes_read_ += bytes;
        while (controller_.hasPendingRequests()) controller_.tick();   // drain to steady state
    }
    uint64_t dramNs() const { return controller_.getMaxChannelTime(); }  // DRAM active time (ns)
    double   tCK()   const { return controller_.tCK(); }                 // ns per DRAM cycle
    long long bytesRead()    const { return bytes_read_; }
    long long bytesWritten() const { return bytes_written_; }
private:
    static constexpr uint64_t kBurstBytes = 64;
    RamulatorHBM controller_;            // SOTA HBM4 (config/hbm4_sota.yaml), per-burst timing
    uint64_t addr_ = 0;                  // running byte address (striped by Ramulator's mapping)
    int      id_   = 0;
    long long bytes_read_ = 0, bytes_written_ = 0;
};

// ---- ground-truth helpers (small n only) ----
template <typename DiagMap>
inline void fillDense(DenseMat& M, const DiagMap& diag, int n) {
    for (const auto& [off, entries] : diag)
        for (const auto& [v, i, j] : entries)
            if (i >= 0 && i < n && j >= 0 && j < n) M[i][j] = v;
}
inline DenseMat denseMatMul(const DenseMat& A, const DenseMat& B, int n) {
    DenseMat C(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k) {
            const double a = A[i][k];
            if (a == 0.0) continue;
            const double* brow = B[k].data(); double* crow = C[i].data();
            for (int j = 0; j < n; ++j) crow[j] += a * brow[j];
        }
    return C;
}
inline long long bytesOf(const ConvDiag& M) {
    long long b = 0;
    for (const auto& [o, e] : M) b += static_cast<long long>(e.size()) * sizeof(DataPackage);
    return b;
}
// Shared on-chip buffer model, applied IDENTICALLY to every accelerator: A tile + B + C
// stage on-chip; the part of the working set beyond kScratchpadBytes spills to HBM
// (written out + read back). Adds the per-matmul HBM traffic and tracks peak buffer use.
inline void chargeHBM(long long bytesA, long long bytesB, long long bytesC,
                      long long& hbm_bytes, long long& peak_foot, long long& spill_bytes) {
    const long long foot = bytesA + bytesB + bytesC;               // resident working set
    const long long overflow = std::max(0LL, foot - kScratchpadBytes);
    hbm_bytes  += bytesA + bytesB + bytesC + 2 * overflow;         // read A,B; write C; spill out+back
    spill_bytes += 2 * overflow;
    peak_foot   = std::max(peak_foot, foot);
}

// ---- connected-mesh reduction map + tile runner (verbatim logic from HBMHamiltonian) ----
inline std::vector<std::vector<int>> generateReductionMap(
        const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
        std::vector<DiagonalReduction*>& reductions, std::ostream& out) {
    size_t col = A_offsets.size(), row = B_offsets.size();
    std::vector<std::vector<int>> reductionMap(row, std::vector<int>(col, -1));
    std::unordered_map<int, DiagonalReduction*> reducerMap;
    std::vector<int> Brev = B_offsets; std::reverse(Brev.begin(), Brev.end());
    for (size_t i = 0; i < row; ++i)
        for (size_t j = 0; j < col; ++j) {
            int index = A_offsets[j] + Brev[i];
            reductionMap[i][j] = index;
            if (!reducerMap.count(index)) {
                auto* r = new DiagonalReduction(index, out);
                reducerMap[index] = r; reductions.push_back(r);
            }
        }
    return reductionMap;
}

// One connected-mesh tile: real PE Grid + reductions, A injected top (cols, pre-sorted by
// index2), B injected left (rows, pre-sorted by index1); merge-join matches A.index2==B.index1,
// product carries (A.index1,B.index2). All routing/buffering/reduction MEASURED via the
// global PE::/DiagonalReduction:: counters. Returns cycles; accumulates output-keyed C.
inline long runMeshTile(const std::vector<int>& A_labels, const std::vector<int>& B_labels,
                        std::vector<std::vector<DataPackage>> A_pk,
                        std::vector<std::vector<DataPackage>> B_pk,
                        std::map<std::pair<int,int>, double>& out_C) {
    static std::ofstream devnull;
    const int COL = (int)A_pk.size(), ROW = (int)B_pk.size();
    if (COL == 0 || ROW == 0) return 0;
    std::vector<DiagonalReduction*> reductions;
    auto reductionMap = generateReductionMap(A_labels, B_labels, reductions, devnull);
    Grid grid(ROW, COL, reductions, reductionMap, devnull, 20000);
    std::vector<Connection*> left_in(ROW), top_in(COL);
    for (int i = 0; i < ROW; ++i) left_in[i] = new Connection(devnull);
    for (int j = 0; j < COL; ++j) top_in[j] = new Connection(devnull);
    grid.setInputConnections(top_in, left_in);
    std::reverse(B_pk.begin(), B_pk.end());
    int cycle = 0;
    std::vector<int> A_idx(COL, 0), B_idx(ROW, 0);
    while (true) {
        for (int col = 0; col < COL; ++col)
            if (cycle >= col && A_idx[col] < (int)A_pk[col].size() && !top_in[col]->pendingSrc()) {
                top_in[col]->receiveSrc(A_pk[col][A_idx[col]]);
                if (++A_idx[col] == (int)A_pk[col].size()) top_in[col]->receiveInjectionFinished(true);
            }
        for (int row = 0; row < ROW; ++row)
            if (cycle >= row && B_idx[row] < (int)B_pk[row].size() && !left_in[row]->pendingSrc()) {
                left_in[row]->receiveSrc(B_pk[row][B_idx[row]]);
                if (++B_idx[row] == (int)B_pk[row].size()) left_in[row]->receiveInjectionFinished(true);
            }
        bool done = true;
        for (int i = 0; i < ROW; ++i) if (left_in[i]->pendingSrc()) done = false;
        for (int j = 0; j < COL; ++j) if (top_in[j]->pendingSrc()) done = false;
        grid.cycle(cycle);
        if (done && grid.isIdle()) break;
        ++cycle;
    }
    for (auto& [idx, entries] : grid.getResults())
        for (auto& [v, i1, i2] : entries) out_C[{i1, i2}] += v;
    for (auto* c : left_in) delete c;
    for (auto* c : top_in) delete c;
    for (auto* r : reductions) delete r;
    return cycle;
}

inline ConvDiag fromCacc(const std::map<std::pair<int,int>, double>& Cacc) {
    ConvDiag C;
    for (const auto& [key, val] : Cacc) {
        if (val == 0.0) continue;
        C[key.second - key.first].emplace_back(val, key.first, key.second);
    }
    for (auto& [o, e] : C)
        std::sort(e.begin(), e.end(), [](auto& a, auto& b){ return std::get<1>(a) < std::get<1>(b); });
    return C;
}

// ================= BREAKDOWN (shared pipelined model, extracted verbatim) =============
struct Breakdown {
    long long compute_cycles = 0, real_cycles = 0;
    double pe_util = 0, mac_pct = 0, mem_pct = 0, router_pct = 0, buffer_pct = 0, accum_pct = 0;
    long long mac = 0, compare = 0, router = 0, buf = 0, accum = 0;
    long long buffer_budget = kScratchpadBytes, buffer_peak = 0, spill_bytes = 0, hbm_bytes = 0;
    double dram_ns = 0, exposed_ns = 0;   // real HBM4 active time (Ramulator2) and its exposed part
    std::string bottleneck; bool verified = false;
};
inline Breakdown computeBreakdown(int grid_row, int grid_col, long long base_cycles,
                                  long long hbm_bytes, long long peak_foot, long long spill_bytes,
                                  double dram_ns = 0.0,
                                  long long add_router = 0, long long add_accum = 0, long long add_bufwr = 0) {
    Breakdown b;
    const uint64_t mac = PE::macCount(), comp = PE::compareCount();
    const uint64_t rout = PE::routerCount() + add_router, bwr = PE::bufWriteCount() + add_bufwr,
                   brd = PE::bufReadCount(), accum = DiagonalReduction::accumulationCount() + add_accum;
    const long long PEs = (long long)grid_row * grid_col;
    const int BUF_PORTS = 2, LINKS_PE = 2;
    auto cdiv = [](double a, double c){ return c > 0 ? std::ceil(a / c) : 0.0; };
    // base_cycles ALREADY folds per-tile reduction (each kernel takes max(tile_compute,
    // tile_products/S) per tile), so reduction is no longer a separate global floor -- it is
    // part of mac_cyc. The remaining on-chip stages (operand buffer, NoC) still overlap.
    const double mac_cyc = (double)base_cycles;
    const double buf_cyc = cdiv((double)(bwr + brd), (double)PEs * BUF_PORTS);
    const double rout_cyc = cdiv((double)rout, (double)PEs * LINKS_PE);
    const double steady = std::max(mac_cyc, std::max(buf_cyc, rout_cyc));
    const double filldrain = (double)(grid_row + grid_col) + std::ceil(std::log2(std::max(2, grid_row)));
    const double pipe = steady + filldrain;                    // on-chip pipeline time (cyc == ns @ 1 GHz)
    // Real HBM4 latency (Ramulator 2.1) folded in the double-buffered way HBMHamiltonian does:
    // DRAM overlaps compute up to the pipeline length; only the exposed remainder adds cycles.
    const double hidden_ns  = std::min(pipe, dram_ns);
    const double exposed_ns = dram_ns - hidden_ns;
    const double real_cyc = pipe + exposed_ns;
    b.bottleneck = (exposed_ns > 0.0) ? "memory" :
                   (steady==mac_cyc) ? "compute" :
                   (steady==buf_cyc) ? "buffer" :
                   (steady==rout_cyc)? "router" : "compute";
    const double E_MAC=1, E_BUF=1, E_ROUTER=2, E_ACCUM=1, E_MEM=200;
    const double mem_acc = (double)hbm_bytes / 32.0;
    const double e_mac=E_MAC*mac, e_buf=E_BUF*(bwr+brd), e_rout=E_ROUTER*rout, e_acc=E_ACCUM*accum, e_mem=E_MEM*mem_acc;
    const double e_tot = e_mac+e_buf+e_rout+e_acc+e_mem;
    auto pct = [&](double e){ return e_tot > 0 ? 100.0*e/e_tot : 0.0; };
    b.dram_ns = dram_ns; b.exposed_ns = exposed_ns;
    b.compute_cycles = base_cycles; b.real_cycles = (long long)real_cyc;
    b.pe_util = (PEs>0 && real_cyc>0) ? 100.0*(double)mac/((double)PEs*real_cyc) : 0.0;
    b.mac_pct=pct(e_mac); b.mem_pct=pct(e_mem); b.router_pct=pct(e_rout); b.buffer_pct=pct(e_buf); b.accum_pct=pct(e_acc);
    b.mac=mac; b.compare=comp; b.router=rout; b.buf=brd+bwr; b.accum=accum;
    b.hbm_bytes=hbm_bytes; b.buffer_peak=peak_foot; b.spill_bytes=spill_bytes;
    return b;
}

// ================= DATAFLOW KERNELS (true cycle-accurate on the shared mesh) ==========
// DIAMOND (ours): diagonal fibers through the connected merge-join mesh.
inline ConvDiag diamondMesh(const ConvDiag& A, const ConvDiag& B, int n, int S, long long& cyc_out) {
    std::vector<int> Aoff, Boff;
    for (const auto& [o,e] : A) Aoff.push_back(o);
    for (const auto& [o,e] : B) Boff.push_back(o);
    std::sort(Aoff.begin(), Aoff.end()); std::sort(Boff.begin(), Boff.end());
    std::map<std::pair<int,int>, double> Cacc;
    long long cyc = 0;
    for (size_t ta = 0; ta < Aoff.size(); ta += S)
        for (size_t tb = 0; tb < Boff.size(); tb += S) {
            size_t amax = std::min(Aoff.size(), ta+S), bmax = std::min(Boff.size(), tb+S);
            std::vector<int> A_labels, B_labels;
            std::vector<std::vector<DataPackage>> A_pk, B_pk;
            for (size_t j = ta; j < amax; ++j) {
                int o = Aoff[j]; A_labels.push_back(o);
                auto e = A.at(o);                                  // entries (val,row,col); col=row+o
                std::sort(e.begin(), e.end(), [](auto&a, auto&b){ return std::get<1>(a) < std::get<1>(b); });
                std::vector<DataPackage> pk;
                for (auto& [v,r,c] : e) pk.emplace_back(v, r, c);  // A sorted by index2=col (=row+o) ✓
                A_pk.push_back(std::move(pk));
            }
            for (size_t i = tb; i < bmax; ++i) {
                int o = Boff[i]; B_labels.push_back(o);
                auto e = B.at(o);
                std::sort(e.begin(), e.end(), [](auto&a, auto&b){ return std::get<1>(a) < std::get<1>(b); });
                std::vector<DataPackage> pk;
                for (auto& [v,r,c] : e) pk.emplace_back(v, r, c);  // B sorted by index1=row ✓
                B_pk.push_back(std::move(pk));
            }
            const long long m0 = (long long)PE::macCount();
            const long tc = runMeshTile(A_labels, B_labels, std::move(A_pk), std::move(B_pk), Cacc);
            const long long tp = (long long)PE::macCount() - m0;              // products this tile
            // Per-tile reduction overlapped with per-tile compute: the tile's makespan is the
            // slower of its compute walk and its reduction (S adder-tree lanes, 1 sum/lane/cyc).
            const long long tred = (long long)std::ceil((double)tp / (double)std::max(1, S));
            cyc += std::max<long long>((long long)tc, tred) + 2*S;
        }
    cyc_out = cyc;
    return fromCacc(Cacc);
}

// TPU: dense — densify operands to all 2n-1 diagonals (real values, 0 where absent), then
// run the SAME diagonal mesh so it cannot exploit sparsity.
inline ConvDiag densify(const ConvDiag& M, int n) {
    std::unordered_map<long long,double> val;
    for (const auto& [o,e] : M) for (const auto& [v,r,c] : e)
        val[((long long)r<<32)|(unsigned)c] = v;
    ConvDiag D;
    for (int o = -(n-1); o <= n-1; ++o) {
        auto& e = D[o]; int len = n - std::abs(o);
        for (int k = 0; k < len; ++k) {
            int r = o>=0?k:k-o, c = o>=0?k+o:k;
            auto it = val.find(((long long)r<<32)|(unsigned)c);
            e.emplace_back(it!=val.end()?it->second:0.0, r, c);
        }
    }
    return D;
}

// Trapezoid MS: inner-product on the connected mesh (X row-fibers x Y col-fibers).
inline ConvDiag trapInnerProductMesh(const ConvDiag& X, const ConvDiag& Y, int n, int S, long long& cyc_out) {
    std::unordered_map<int, std::vector<std::pair<int,double>>> Xrow, Ycol;
    for (const auto& [o,e]:X) for (const auto& [v,r,c]:e) Xrow[r].emplace_back(c, v);
    for (const auto& [o,e]:Y) for (const auto& [v,r,c]:e) Ycol[c].emplace_back(r, v);
    for (auto& [m,f]:Xrow) std::sort(f.begin(), f.end());
    for (auto& [nn,f]:Ycol) std::sort(f.begin(), f.end());
    std::vector<int> rows, cols;
    for (auto& [m,f]:Xrow) rows.push_back(m);
    for (auto& [nn,f]:Ycol) cols.push_back(nn);
    std::sort(rows.begin(),rows.end()); std::sort(cols.begin(),cols.end());
    std::map<std::pair<int,int>, double> Cacc;
    long long cyc = 0;
    for (size_t tj = 0; tj < cols.size(); tj += S)
        for (size_t ti = 0; ti < rows.size(); ti += S) {
            size_t jmax = std::min(cols.size(), tj+S), imax = std::min(rows.size(), ti+S);
            std::vector<int> A_labels, B_labels;
            std::vector<std::vector<DataPackage>> A_pk, B_pk;
            for (size_t jj = tj; jj < jmax; ++jj) {
                int nn = cols[jj]; A_labels.push_back(nn);
                std::vector<DataPackage> pk;
                for (const auto& [k,v] : Ycol[nn]) pk.emplace_back(v, nn, k);   // sorted by index2=k
                A_pk.push_back(std::move(pk));
            }
            for (size_t ii = ti; ii < imax; ++ii) {
                int m = rows[ii]; B_labels.push_back(m);
                std::vector<DataPackage> pk;
                for (const auto& [k,v] : Xrow[m]) pk.emplace_back(v, k, m);     // sorted by index1=k
                B_pk.push_back(std::move(pk));
            }
            const long long m0 = (long long)PE::macCount();
            const long tc = runMeshTile(A_labels, B_labels, std::move(A_pk), std::move(B_pk), Cacc);
            const long long tp = (long long)PE::macCount() - m0;              // products this tile
            const long long tred = (long long)std::ceil((double)tp / (double)std::max(1, S));
            cyc += std::max<long long>((long long)tc, tred) + 2*S;            // per-tile reduction overlap
        }
    cyc_out = cyc;
    // product carries (nn,m): swap to (m,nn) diagonal form.
    ConvDiag C;
    for (auto& [key,val] : Cacc) { if (val==0.0) continue; int nn=key.first, m=key.second; C[nn-m].emplace_back(val,m,nn); }
    for (auto& [o,e] : C) std::sort(e.begin(), e.end(), [](auto&a, auto&b){ return std::get<1>(a) < std::get<1>(b); });
    return C;
}

// Trapezoid HS: Gustavson (expand-merge) on real (isolated) PEs; routing/accumulate are
// COMPUTED addends because Gustavson's broadcast-multiply is a different datapath than the
// merge-join mesh (documented limitation).
inline ConvDiag trapGustavson(const ConvDiag& A, const ConvDiag& B, int n, int S,
                              long long& cyc_out, long long& router_out, long long& accum_out, long long& bufwr_out) {
    static std::ofstream devnull;
    std::unordered_map<int, std::vector<std::pair<int,double>>> Arow, Brow;
    for (const auto& [o,e]:A) for (const auto& [v,r,c]:e) Arow[r].emplace_back(c, v);
    for (const auto& [o,e]:B) for (const auto& [v,r,c]:e) Brow[r].emplace_back(c, v);
    // De-idealized cost model (Gustavson CANNOT run on the merge-join connected mesh -- it is
    // broadcast-multiply, not fiber intersection -- so its NoC/buffer traffic is charged
    // explicitly here rather than measured, at the SAME intensity the connected mesh exhibits,
    // so HS is not flattered by its isolated datapath):
    //  - kHopsPerProduct: each product is scattered from its expand unit to the output-column
    //    accumulator across a multi-hop NoC path. The measured merge-join mesh routes ~3
    //    hops/MAC, so charge 3 (do NOT undercount as 1).
    //  - buffer: B-row streaming (expand) + a read-modify-write per product for the irregular
    //    merge into the output accumulator (2 buffer accesses/product).
    constexpr long long kHopsPerProduct = 3;
    std::map<std::pair<int,int>, std::vector<std::pair<double,double>>> prod;
    long long router = 0, bufwr = 0;
    for (const auto& [m, af] : Arow)
        for (const auto& [k, aval] : af) {
            auto it = Brow.find(k); if (it == Brow.end()) continue;
            bufwr += (long long)it->second.size();                        // stream B-row-k (expand)
            for (const auto& [nn, bval] : it->second) {
                prod[{m,nn}].emplace_back(aval,bval);
                router += kHopsPerProduct;                                // multi-hop scatter to column accumulator
                bufwr  += 2;                                              // irregular merge: read+write the accumulator
            }
        }
    std::set<int> rowset, colset;
    for (const auto& [mn,_] : prod) { rowset.insert(mn.first); colset.insert(mn.second); }
    std::vector<int> rows(rowset.begin(), rowset.end()), cols(colset.begin(), colset.end());
    std::map<std::pair<int,int>, double> Cacc;
    long long cyc = 0, accum = 0;
    for (size_t ti = 0; ti < rows.size(); ti += S)
        for (size_t tj = 0; tj < cols.size(); tj += S) {
            std::vector<std::unique_ptr<PE>> pes;
            size_t imax = std::min(rows.size(), ti+S), jmax = std::min(cols.size(), tj+S);
            for (size_t ii = ti; ii < imax; ++ii)
                for (size_t jj = tj; jj < jmax; ++jj) {
                    auto pit = prod.find({rows[ii], cols[jj]});
                    if (pit == prod.end() || pit->second.empty()) continue;
                    int m = rows[ii], nn = cols[jj];
                    auto pe = std::make_unique<PE>(0, 0, devnull, pit->second.size() + 16);
                    pe->setLastRow(true); pe->setLastCol(true);
                    int key = 0;
                    for (const auto& [aval,bval] : pit->second) {
                        pe->receivedA.push(DataPackage(aval, m, key));
                        pe->receivedB.push(DataPackage(bval, key, nn)); ++key;
                    }
                    pes.push_back(std::move(pe));
                }
            const long long m0 = (long long)PE::macCount();
            long tile_cyc = 0; bool any = true;
            while (any) { any = false;
                for (auto& pe : pes) if (!pe->receivedA.isEmpty() && !pe->receivedB.isEmpty()) { pe->cycle((uint64_t)tile_cyc); any = true; }
                if (any) ++tile_cyc; }
            const long long tp = (long long)PE::macCount() - m0;              // products this tile
            const long long tred = (long long)std::ceil((double)tp / (double)std::max(1, S));
            if (!pes.empty()) cyc += std::max<long long>(tile_cyc, tred) + 2*S;  // per-tile reduction overlap
            for (auto& pe : pes) while (!pe->PsumOut.isEmpty()) {
                DataPackage r = pe->PsumOut.front(); pe->PsumOut.pop();
                Cacc[{r.index1, r.index2}] += r.value; ++accum;
            }
        }
    cyc_out = cyc; router_out = router; accum_out = accum; bufwr_out = bufwr;
    return fromCacc(Cacc);
}

// ================= ACCELERATOR CLASSES (common interface) =============================
struct Accelerator {
    virtual ~Accelerator() {}
    virtual const char* name() const = 0;
    // C = A@B on this accelerator's dataflow. Adds cycles/traffic/peak into the caller's
    // accumulators; add_* carry any distribution/accumulate NOT captured by the global
    // PE:: counters (nonzero only for the isolated Gustavson path). `hbm` is the SAME
    // Ramulator2 HBM4 model for every accelerator; each charges the SAME per-iter traffic
    // (read prev power + read H + write C) through it so the DRAM timing is real and fair.
    virtual ConvDiag matmul(const ConvDiag& A, const ConvDiag& B, int n, int S,
                            long long& cyc, long long& add_router, long long& add_accum,
                            long long& add_bufwr, long long& hbm_bytes, long long& peak_foot,
                            long long& spill_bytes, AccelHBM& hbm) = 0;
};

// Charge one matmul's operand/result traffic to BOTH the byte/peak/spill accountant (energy
// + buffer model) and the real Ramulator2 DRAM (timing). Identical pattern for all four.
inline void chargeTraffic(long long bA, long long bB, long long bC,
                          long long& hbm_bytes, long long& peak, long long& spill, AccelHBM& hbm) {
    chargeHBM(bA, bB, bC, hbm_bytes, peak, spill);      // bytes/peak/spill (energy + buffer)
    hbm.transfer(bA, /*isWrite=*/false);                // read prev power
    hbm.transfer(bB, /*isWrite=*/false);                // read H
    hbm.transfer(bC, /*isWrite=*/true);                 // write new power
}

struct DiamondAccel : Accelerator {
    const char* name() const override { return "DIAMOND (diagonal-ours)"; }
    ConvDiag matmul(const ConvDiag& A, const ConvDiag& B, int n, int S, long long& cyc,
                    long long&, long long&, long long&, long long& hbm, long long& peak,
                    long long& spill, AccelHBM& mem) override {
        long long c = 0; ConvDiag C = diamondMesh(A, B, n, S, c); cyc += c;
        chargeTraffic(bytesOf(A), bytesOf(B), bytesOf(C), hbm, peak, spill, mem);
        return C;
    }
};
struct TpuAccel : Accelerator {
    const char* name() const override { return "TPU (dense MXU)"; }
    ConvDiag matmul(const ConvDiag& A, const ConvDiag& B, int n, int S, long long& cyc,
                    long long&, long long&, long long&, long long& hbm, long long& peak,
                    long long& spill, AccelHBM& mem) override {
        ConvDiag Ad = densify(A, n), Bd = densify(B, n);
        long long c = 0; ConvDiag C = diamondMesh(Ad, Bd, n, S, c); cyc += c;
        chargeTraffic(bytesOf(Ad), bytesOf(Bd), bytesOf(Ad), hbm, peak, spill, mem);  // dense operands + dense output
        return C;
    }
};
struct TrapMSAccel : Accelerator {
    const char* name() const override { return "Trapezoid-MS (inner-product)"; }
    ConvDiag matmul(const ConvDiag& A, const ConvDiag& B, int n, int S, long long& cyc,
                    long long&, long long&, long long&, long long& hbm, long long& peak,
                    long long& spill, AccelHBM& mem) override {
        long long c = 0; ConvDiag C = trapInnerProductMesh(A, B, n, S, c); cyc += c;
        chargeTraffic(bytesOf(A), bytesOf(B), bytesOf(C), hbm, peak, spill, mem);
        return C;
    }
};
struct TrapHSAccel : Accelerator {
    const char* name() const override { return "Trapezoid-HS (Gustavson)"; }
    ConvDiag matmul(const ConvDiag& A, const ConvDiag& B, int n, int S, long long& cyc,
                    long long& ar, long long& aa, long long& ab, long long& hbm, long long& peak,
                    long long& spill, AccelHBM& mem) override {
        long long c = 0, r = 0, a = 0, b = 0;
        ConvDiag C = trapGustavson(A, B, n, S, c, r, a, b);
        cyc += c; ar += r; aa += a; ab += b;
        chargeTraffic(bytesOf(A), bytesOf(B), bytesOf(C), hbm, peak, spill, mem);
        return C;
    }
};

// ---- minimal DIA loader (hamlib/dia_oom/*.txt) -> ConvDiag; returns n or -1 ----
inline long loadDia(const std::string& path, ConvDiag& diag) {
    std::ifstream in(path); if (!in) return -1;
    std::string first; if (!std::getline(in, first)) return -1;
    long n = 0; { std::istringstream iss(first); std::string t;
        if (!(iss >> t) || t != "N") return -1; if (!(iss >> n)) return -1; }
    if (n <= 0) return -1;
    diag.clear(); std::string line;
    while (std::getline(in, line)) {
        size_t colon = line.find(':'); if (colon == std::string::npos) continue;
        int off = std::stoi(line.substr(0, colon));
        long len = n - std::labs((long)off); if (len <= 0) continue;
        std::vector<std::tuple<double,int,int>> ents;
        const char* p = line.c_str() + colon + 1; char* end = nullptr;
        for (long k = 0; k < len; ++k) {
            double v = std::strtod(p, &end); if (end == p) break; p = end;
            if (v != 0.0) {
                int row = off >= 0 ? (int)k : (int)(k - off), col = off >= 0 ? (int)(k + off) : (int)k;
                ents.emplace_back(v, row, col);
            }
        }
        if (!ents.empty()) diag[off] = std::move(ents);
    }
    return n;
}

} // namespace accel
