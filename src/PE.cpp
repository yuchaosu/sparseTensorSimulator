#include "../include/PE.h"
#include <chrono>
#include <string>
#include <algorithm>

PE::PE(int row, int col, std::ostream& output_stream, size_t fifo_depth)
    : receivedA(fifo_depth), receivedB(fifo_depth), r(row), c(col), out(output_stream) {
    // receivedA/receivedB model the finite per-PE input buffers (fixed depth).
    // PsumOut keeps its large default: in the HBM datapath it is only ever fed by
    // matches (<=1/cycle) and drained by the reduction each cycle, so it stays O(1)
    // and never needs match-time backpressure.
    connection_top = nullptr;
    connection_bottom = nullptr;
    connection_left = nullptr;
    connection_right = nullptr;
    out << "PE created at (" << r << ", " << c << ")\n";
}

// Static tracing state
static bool g_compute_trace_enabled = false;
static FILE* g_compute_trace_fp = nullptr;
// Aggregate compute tracing state
static bool g_compute_trace_agg_enabled = false;
static FILE* g_compute_trace_agg_fp = nullptr;
static uint64_t g_compute_agg_flops = 0;
static uint64_t g_total_multiplies = 0;   // always-on scalar-multiply counter (debug)
// Peak input-FIFO occupancy (entries), gated so the ideal/unbounded baseline pass
// does not inflate the realistic peak.
static uint64_t g_peak_occ = 0;
static bool g_peak_enabled = true;
static uint64_t g_compute_agg_events = 0;
static uint64_t g_compute_agg_first_ts = 0;
static uint64_t g_compute_agg_last_ts = 0;

// True per-component activity counters (aggregated over all PEs), incremented inside
// the real datapath for the energy/cycle breakdown. See PE.h.
static uint64_t g_act_compare = 0;   // comparator evaluations
static uint64_t g_act_router  = 0;   // neighbour/psum sends (NoC hops)
static uint64_t g_act_buf_wr  = 0;   // FIFO pushes
static uint64_t g_act_buf_rd  = 0;   // FIFO pops

void PE::enableComputeTrace(const std::string& path) {
    if (g_compute_trace_enabled) return;
    g_compute_trace_fp = std::fopen(path.c_str(), "w");
    if (!g_compute_trace_fp) {
        std::cerr << "Failed to open compute trace file: " << path << std::endl;
        g_compute_trace_enabled = false;
        return;
    }
    std::fprintf(g_compute_trace_fp, "timestamp_ns,pe_r,pe_c,evt,flops,idx1,idx2,k\n");
    std::fflush(g_compute_trace_fp);
    g_compute_trace_enabled = true;
}

void PE::enableComputeTraceAggregate(const std::string& path) {
    if (g_compute_trace_agg_enabled) return;
    g_compute_trace_agg_fp = std::fopen(path.c_str(), "w");
    if (!g_compute_trace_agg_fp) {
        std::cerr << "Failed to open compute aggregate trace file: " << path << std::endl;
        g_compute_trace_agg_enabled = false;
        return;
    }
    std::fprintf(g_compute_trace_agg_fp, "total_flops,events,first_time_ns,last_time_ns\n");
    std::fflush(g_compute_trace_agg_fp);
    g_compute_agg_flops = 0;
    g_compute_agg_events = 0;
    g_compute_agg_first_ts = 0;
    g_compute_agg_last_ts = 0;
    g_compute_trace_agg_enabled = true;
}

void PE::disableComputeTraceAggregate() {
    if (!g_compute_trace_agg_fp) return;
    std::fprintf(g_compute_trace_agg_fp, "%llu,%llu,%llu,%llu\n",
                 static_cast<unsigned long long>(g_compute_agg_flops),
                 static_cast<unsigned long long>(g_compute_agg_events),
                 static_cast<unsigned long long>(g_compute_agg_first_ts),
                 static_cast<unsigned long long>(g_compute_agg_last_ts));
    std::fflush(g_compute_trace_agg_fp);
    std::fclose(g_compute_trace_agg_fp);
    g_compute_trace_agg_fp = nullptr;
    g_compute_trace_agg_enabled = false;
}

bool PE::isComputeTraceAggregateEnabled() {
    return g_compute_trace_agg_enabled;
}

uint64_t PE::totalMultiplies() { return g_total_multiplies; }

void PE::resetActivity() {
    g_total_multiplies = 0; g_act_compare = 0; g_act_router = 0; g_act_buf_wr = 0; g_act_buf_rd = 0;
}
uint64_t PE::macCount()      { return g_total_multiplies; }
uint64_t PE::compareCount()  { return g_act_compare; }
uint64_t PE::routerCount()   { return g_act_router; }
uint64_t PE::bufWriteCount() { return g_act_buf_wr; }
uint64_t PE::bufReadCount()  { return g_act_buf_rd; }

void PE::resetPeakOccupancy() { g_peak_occ = 0; }
void PE::setPeakTracking(bool on) { g_peak_enabled = on; }
uint64_t PE::peakOccupancy() { return g_peak_occ; }

void PE::disableComputeTrace() {
    if (!g_compute_trace_enabled) return;
    if (g_compute_trace_fp) {
        std::fclose(g_compute_trace_fp);
        g_compute_trace_fp = nullptr;
    }
    g_compute_trace_enabled = false;
}

bool PE::isComputeTraceEnabled() {
    return g_compute_trace_enabled;
}


void PE::setLastRow(bool last) { last_row = last; }
void PE::setLastCol(bool last) { last_col = last; }

void PE::setTopConnection(Connection* conn) {
    connection_top = conn;
}

void PE::setLeftConnection(Connection* conn) {
    connection_left = conn;
}
void PE::setRightConnection(Connection* conn) {
    connection_right = conn;
}
void PE::setBottomConnection(Connection* conn) {
    connection_bottom = conn;
}


void PE::sendBottom() {
    if (!receivedA.isEmpty() && connection_bottom) {
        DataPackage val = receivedA.front();
        if (!connection_bottom->pendingSrc()) {
            // If the bottom connection is not pending, we can send the value
            connection_bottom->receiveSrc(val);
            out << "PE (" << r << ", " << c << ") sending to bottom: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
            sends++; g_act_router++;
        } else {
            // If the bottom connection is pending, we might need to handle it differently
            // For now, we just print a message
            out << "PE (" << r << ", " << c << ") cannot send to bottom yet, waiting for previous data to be processed.\n";
        }
    }
}

void PE::sendPsum() {
    // Only pop once the psum channel actually accepts the value; otherwise the
    // reduction has not drained the previous psum yet and popping here would
    // silently drop this result. (The reduction drains every cycle, so this holds
    // for at most one cycle, but the guard is required for correctness under any
    // backpressure.)
    if (!PsumOut.isEmpty() && connection_bottom && !connection_bottom->pendingPsum()) {
        DataPackage val = PsumOut.front();
        connection_bottom->receivePsum(val);
        out << "PE (" << r << ", " << c << ") sending Psum to bottom: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
        PsumOut.pop(); g_act_buf_rd++;
        sends++; g_act_router++;
    }
}


void PE::sendRight() {
    if (!receivedB.isEmpty() && connection_right) {
        DataPackage val = receivedB.front();
        if( !connection_right->pendingSrc()) {
            // If the right connection is not pending, we can send the value
            connection_right->receiveSrc(val);
            out << "PE (" << r << ", " << c << ") sending to right: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
            sends++; g_act_router++;
        } else {
            // If the right connection is pending, we might need to handle it differently
            // For now, we just print a message
            out << "PE (" << r << ", " << c << ") cannot send to right yet, waiting for previous data to be processed.\n";
        }
        
    }
}


void PE::receive() {
    // The per-PE input FIFO is a small static pipeline buffer; the large operand
    // buffering / merge-join alignment is done by the edge scratchpad that feeds
    // the array (see CScratchpad). So the PE always accepts the datum the mesh
    // hands it (no per-PE backpressure — that would try to make this tiny FIFO
    // absorb the O(max-offset) skew and deadlocks). Peak occupancy is tracked to
    // report the scratchpad staging requirement.
    if (connection_top && connection_top->pendingSrc()) {
        DataPackage src = connection_top->sendSrc();
        //if (src.value != INT_MIN) {
            receivedA.push(src);
            out << "PE (" << r << ", " << c << ") received A from top: " << src.value << " \t index1: " << src.index1 << " \t index2: " << src.index2 << "\n";

            receives++; g_act_buf_wr++;


    }

    if (connection_left && connection_left->pendingSrc()) {
        DataPackage psum = connection_left->sendSrc();
        //if (psum.value != INT_MIN) {
            receivedB.push(psum);
            out << "PE (" << r << ", " << c << ") received B from left: " << psum.value << " \t index1: " << psum.index1 << " \t index2: " << psum.index2 << "\n";

            receives++; g_act_buf_wr++;


    }

    if (g_peak_enabled) {
        uint64_t occ = std::max(receivedA.size(), receivedB.size());
        if (occ > g_peak_occ) g_peak_occ = occ;
    }

    // if (connection_top && connection_top->pendingPsum()) {
    //     DataPackage psum = connection_top->sendPsum();
    //     if (psum.value != INT_MIN) {
    //         PsumOut.push(psum);
    //         out << "PE (" << r << ", " << c << ") received Psum from top: " << psum.value << " \t index1: " << psum.index1 << " \t index2: " << psum.index2 << "\n";
    //     }
    // }

    if (connection_top && connection_top->pendingTransfer()) {
        DataPackage transfer = connection_top->sendTransfer();
        //if (transfer.value != INT_MIN) {
            PsumOut.push(transfer);
            out << "PE (" << r << ", " << c << ") received Transfer from top: " << transfer.value << " \t index1: " << transfer.index1 << " \t index2: " << transfer.index2 << "\n";

            receives++; g_act_buf_wr++;


    }

    // Latch stream-exhaustion tokens: once the top (A) / left (B) stream signals
    // it is finished, no further operands for this PE will arrive on it.
    if (connection_top && connection_top->pendingInjectionFinished()) {
        if (connection_top->isInjectionFinished()) injection_finished_top = true;
    }
    if (connection_left && connection_left->pendingInjectionFinished()) {
        if (connection_left->isInjectionFinished()) injection_finished_left = true;
    }
}
void PE::cycle(uint64_t cycle) {
    idle = true; // Reset idle state at the start of the cycle

    if (!receivedA.isEmpty() || !receivedB.isEmpty() || !PsumOut.isEmpty()) {
        idle = false; // If there is any data to process, the PE is not idle
    }

    // Advance a pass-through operand only if it can leave this PE: a boundary PE
    // drains it (no downstream neighbour); an interior PE needs the downstream
    // link free (backpressure).
    const bool topPending  = connection_top  && connection_top->pendingSrc();
    const bool leftPending = connection_left && connection_left->pendingSrc();
    const bool canAdvanceA = last_row ? true : (!connection_bottom || !connection_bottom->pendingSrc());
    const bool canAdvanceB = last_col ? true : (!connection_right  || !connection_right->pendingSrc());

    if (!receivedA.isEmpty() && !receivedB.isEmpty()) {
        DataPackage valueA = receivedA.front();
        DataPackage valueB = receivedB.front();
        if (valueA.index2 == valueB.index1) {
            // Match: both operands advance, so both downstream links must be free.
            if (canAdvanceA && canAdvanceB) {
                DataPackage result = DataPackage(valueA.value * valueB.value, valueA.index1, valueB.index2);
                PsumOut.push(result); g_act_buf_wr++;
                out << "PE (" << r << ", " << c << ") computed multiplication: " << valueA.value << " * " << valueB.value << " = " << result.value << " \t index1: " << valueA.index1 << " \t index2: " << valueB.index2 << "\n";
                // Emit compute trace (1 multiply -> count as 1 FLOP for multiply; adjust if you count MACs)
                uint64_t ts_ns = (cycle * 1000000000ULL) / PE::kClockFrequencyHz;
                if (g_compute_trace_agg_enabled) {
                    g_compute_agg_flops += 1;
                    g_compute_agg_events += 1;
                    if (g_compute_agg_first_ts == 0 || ts_ns < g_compute_agg_first_ts) g_compute_agg_first_ts = ts_ns;
                    if (ts_ns > g_compute_agg_last_ts) g_compute_agg_last_ts = ts_ns;
                }
                if (g_compute_trace_enabled && g_compute_trace_fp) {
                    std::fprintf(g_compute_trace_fp, "%llu,%d,%d,COMPUTE,%d,%d,%d,%d\n",
                                 static_cast<unsigned long long>(ts_ns), r, c, 1, valueA.index1, valueB.index2,
                                 valueA.index2);   // last col = contraction index k (= A.index2 = B.index1)
                    std::fflush(g_compute_trace_fp);
                }
                if (!last_row) sendBottom();
                if (!last_col) sendRight();
                receivedA.pop(); g_act_buf_rd++;
                receivedB.pop(); g_act_buf_rd++;
                multiplies++;
                ++g_total_multiplies;
                compares++; g_act_compare++;
                demux++;
            } // else: stall until both downstream links are free
        } else if (valueA.index2 < valueB.index1) {
            // A's contraction index can never match current/future B; advance A.
            if (canAdvanceA) {
                out << "PE (" << r << ", " << c << ") Index mismatch, advancing A: " << valueA.value << " " << valueA.index1 << " " << valueA.index2 << "\n";
                if (!last_row) sendBottom();
                receivedA.pop(); g_act_buf_rd++;
                compares++; g_act_compare++;
                demux++;
            }
        } else { // valueA.index2 > valueB.index1
            if (canAdvanceB) {
                out << "PE (" << r << ", " << c << ") Index mismatch, advancing B: " << valueB.value << " " << valueB.index1 << " " << valueB.index2 << "\n";
                if (!last_col) sendRight();
                receivedB.pop(); g_act_buf_rd++;
                compares++; g_act_compare++;
                demux++;
            }
        }
    }
    else if (!receivedB.isEmpty()) {
        // Only B present. A matching A may still arrive, so STALL — unless the A
        // stream is exhausted, in which case remaining B can never match here.
        const bool aExhausted = injection_finished_top && !topPending;
        if (aExhausted && canAdvanceB) {
            out << "PE (" << r << ", " << c << ") A stream exhausted, draining B\n";
            if (!last_col) sendRight();
            receivedB.pop(); g_act_buf_rd++;
        }
    }
    else if (!receivedA.isEmpty()) {
        // Only A present. Symmetric: stall unless the B stream is exhausted.
        const bool bExhausted = injection_finished_left && !leftPending;
        if (bExhausted && canAdvanceA) {
            out << "PE (" << r << ", " << c << ") B stream exhausted, draining A\n";
            if (!last_row) sendBottom();
            receivedA.pop(); g_act_buf_rd++;
        }
    }

    // Propagate stream-finished tokens to real downstream neighbours once this
    // PE has fully drained the corresponding input (all its data forwarded).
    if (injection_finished_top && receivedA.isEmpty() && !topPending &&
        !sent_finished_top && !last_row && connection_bottom &&
        !connection_bottom->pendingInjectionFinished()) {
        connection_bottom->receiveInjectionFinished(true);
        sent_finished_top = true;
    }
    if (injection_finished_left && receivedB.isEmpty() && !leftPending &&
        !sent_finished_left && !last_col && connection_right &&
        !connection_right->pendingInjectionFinished()) {
        connection_right->receiveInjectionFinished(true);
        sent_finished_left = true;
    }

    sendPsum();
    receive(); // Receive new data for the next cycle
    


}

Connection* PE::getBottomConnection() {
    return connection_bottom;
}

Connection* PE::getRightConnection() {
    return connection_right;
}

bool PE::isIdle() const {
    return idle;
}

void PE::setIdle(bool idle) {
    this->idle = idle;
}

void PE::printEnergy(std::ostream& out) const {
    out << "Multiplies: " << multiplies << "\n";
    out << "Compares: " << compares << "\n";
    out << "Sends: " << sends << "\n";
    out << "Receives: " << receives << "\n";
    out << "Demux: " << demux << "\n";
}

uint64_t PE::getMultiplyCount() const {
    return multiplies;
}