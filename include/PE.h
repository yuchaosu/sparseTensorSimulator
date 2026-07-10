#ifndef PE_UPDATE_H
#define PE_UPDATE_H

#include <iostream>
#include <optional>
#include <cstdint>
#include "Utility.h"
#include "Fifo.h"
#include "Connection.h"
#include <cstdio>
#include <string>

class PE {
public:
    PE(int row = 0, int col = 0, std::ostream& output_stream = std::cout,
       size_t fifo_depth = 20000);

    static constexpr uint64_t kClockFrequencyHz = 700000000ULL;

    void receive();
    void send();
    void compute();

    void setLastRow(bool last);
    void setLastCol(bool last);

    void setTopConnection(Connection* conn);
    void setLeftConnection(Connection* conn);
    void setRightConnection(Connection* conn);
    void setBottomConnection(Connection* conn);
    void logState(int row, int col, int cycle) const;

    void sendBottom();
    void sendRight();
    void sendPsum();
    void sendIsInjectionFinishedLeft();
    void sendIsInjectionFinishedBottom();
    void finishHandshakeSend();
    void finishHandshakeReceive();
    //void sendTransfer();
    //void sendReceivedPsum();

    void cycle(uint64_t cycle);

    bool isIdle() const;
    void setIdle(bool idle);

    uint64_t getMultiplyCount() const;

    void printEnergy(std::ostream& out) const;

    // Compute tracing (optional)
    static void enableComputeTrace(const std::string& path);
    static void disableComputeTrace();
    static bool isComputeTraceEnabled();
    // Aggregate compute tracing (compact counters)
    static void enableComputeTraceAggregate(const std::string& path);
    static void disableComputeTraceAggregate();
    static bool isComputeTraceAggregateEnabled();
    static uint64_t totalMultiplies();   // global count of scalar multiplies (debug)

    // ---- True (measured) activity counters, aggregated across all PEs, for the
    // per-component energy/cycle breakdown. Every increment happens inside the real
    // cycle-accurate datapath (not a formula): MAC on a match, compare on every
    // comparator eval, router on each neighbour/psum send, buffer read/write on each
    // FIFO pop/push. resetActivity() zeroes them before a run.
    static void     resetActivity();
    static uint64_t macCount();       // matches -> multiply-accumulate
    static uint64_t compareCount();   // merge-join comparator evaluations
    static uint64_t routerCount();    // inter-PE forwards + psum sends (NoC hops)
    static uint64_t bufWriteCount();  // FIFO pushes (operand/psum in)
    static uint64_t bufReadCount();   // FIFO pops (operand/psum out)
    // Peak input-FIFO occupancy (max of receivedA/receivedB size seen, in entries),
    // used to justify the modeled fixed FIFO depth. Tracking is gated so an ideal
    // (unbounded) baseline pass does not pollute the realistic peak.
    static void resetPeakOccupancy();
    static void setPeakTracking(bool on);
    static uint64_t peakOccupancy();

    Connection* getBottomConnection();
    Connection* getRightConnection();
    FIFO<DataPackage> receivedA; //Received data from the top connection
    FIFO<DataPackage> receivedB; //Received data from the left connection
    //FIFO<DataPackage> receivedPsum; //Received partial sums from other PEs
    //FIFO<DataPackage> receivedTransfer; //Received transferred partial sums
    //FIFO<DataPackage> receivedPsumOut; //Output partial sum, combination of Psum and receivedTransfer
    FIFO<DataPackage> PsumOut; //Current partial sum
    //FIFO<DataPackage> transferOut; //Current transfer data

    int r,c;
    Connection* connection_top;
    Connection* connection_bottom;
    Connection* connection_left;
    Connection* connection_right;
    private:
    bool idle = false; // Indicates if the PE is idle
    std::ostream& out;
    bool blocked_A = false; // Indicates if PE is blocked on A
    bool blocked_B = false; // Indicates if PE is blocked on B
    bool sent_A = false; // Indicates if PE has sent A
    bool sent_B = false; // Indicates if PE has sent B
    bool injection_finished_left = false; // Indicates if all data has been injected
    bool injection_finished_top = false; // Indicates if all data has been injected
    bool last_row = false;
    bool last_col = false; // Indicates if this is the last column
    bool sent_finished_left = false; // Indicates if the left connection has been sent
    bool sent_finished_top = false; // Indicates if the top connection has been sent
    bool handshake_finished_right = false; // Indicates if the handshake is finished
    bool handshake_finished_bottom = false; // Indicates if the handshake is finished
    int pre_leftIndex;
    int pre_topIndex;
    uint64_t multiplies = 0; // Number of multiplications performed
    int compares = 0; // Number of comparisons performed
    int sends = 0; // Number of sends performed
    int receives = 0; // Number of receives performed
    int demux =0;
};

#endif // PE_H
