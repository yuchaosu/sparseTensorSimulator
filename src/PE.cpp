#include "../include/PE.h"
#include <chrono>
#include <string>

PE::PE(int row, int col, std::ostream& output_stream) : r(row), c(col), out(output_stream) {
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
static uint64_t g_compute_agg_events = 0;
static uint64_t g_compute_agg_first_ts = 0;
static uint64_t g_compute_agg_last_ts = 0;

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
            sends++;
        } else {
            // If the bottom connection is pending, we might need to handle it differently
            // For now, we just print a message
            out << "PE (" << r << ", " << c << ") cannot send to bottom yet, waiting for previous data to be processed.\n";
        }
    }
}

void PE::sendPsum() {
    if (!PsumOut.isEmpty() && connection_bottom) {
        DataPackage val = PsumOut.front();
        connection_bottom->receivePsum(val);
        out << "PE (" << r << ", " << c << ") sending Psum to bottom: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
        PsumOut.pop();
        sends++;
    }
}


void PE::sendRight() {
    if (!receivedB.isEmpty() && connection_right) {
        DataPackage val = receivedB.front();
        if( !connection_right->pendingSrc()) {
            // If the right connection is not pending, we can send the value
            connection_right->receiveSrc(val);
            out << "PE (" << r << ", " << c << ") sending to right: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
            sends++;
        } else {
            // If the right connection is pending, we might need to handle it differently
            // For now, we just print a message
            out << "PE (" << r << ", " << c << ") cannot send to right yet, waiting for previous data to be processed.\n";
        }
        
    }
}


void PE::receive() {
    if (connection_top && connection_top->pendingSrc()) {
        DataPackage src = connection_top->sendSrc();
        //if (src.value != INT_MIN) {
            receivedA.push(src);
            out << "PE (" << r << ", " << c << ") received A from top: " << src.value << " \t index1: " << src.index1 << " \t index2: " << src.index2 << "\n";

            receives++;
        

    }

    if (connection_left && connection_left->pendingSrc()) {
        DataPackage psum = connection_left->sendSrc();
        //if (psum.value != INT_MIN) {
            receivedB.push(psum);
            out << "PE (" << r << ", " << c << ") received B from left: " << psum.value << " \t index1: " << psum.index1 << " \t index2: " << psum.index2 << "\n";

            receives++;
        

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

            receives++;


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
                PsumOut.push(result);
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
                receivedA.pop();
                receivedB.pop();
                multiplies++;
                ++g_total_multiplies;
                compares++;
                demux++;
            } // else: stall until both downstream links are free
        } else if (valueA.index2 < valueB.index1) {
            // A's contraction index can never match current/future B; advance A.
            if (canAdvanceA) {
                out << "PE (" << r << ", " << c << ") Index mismatch, advancing A: " << valueA.value << " " << valueA.index1 << " " << valueA.index2 << "\n";
                if (!last_row) sendBottom();
                receivedA.pop();
                compares++;
                demux++;
            }
        } else { // valueA.index2 > valueB.index1
            if (canAdvanceB) {
                out << "PE (" << r << ", " << c << ") Index mismatch, advancing B: " << valueB.value << " " << valueB.index1 << " " << valueB.index2 << "\n";
                if (!last_col) sendRight();
                receivedB.pop();
                compares++;
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
            receivedB.pop();
        }
    }
    else if (!receivedA.isEmpty()) {
        // Only A present. Symmetric: stall unless the B stream is exhausted.
        const bool bExhausted = injection_finished_left && !leftPending;
        if (bExhausted && canAdvanceA) {
            out << "PE (" << r << ", " << c << ") B stream exhausted, draining A\n";
            if (!last_row) sendBottom();
            receivedA.pop();
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