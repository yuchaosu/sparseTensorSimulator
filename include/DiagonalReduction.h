#ifndef DIAGONALREDUCTION_H
#define DIAGONALREDUCTION_H

#include "Connection.h"
#include <map>
#include <vector>

class DiagonalReduction {
private:
    std::vector<Connection*> diagonalPorts;
    int index;
    std::map<std::pair<int, int>, double> diagonal;
    std::ostream& out;  // Output stream for logging
    int reduction = 0;  // Counter for reductions

public:
    DiagonalReduction(int index, std::ostream& output_stream);

    void cycle();  // collect one cycle worth of data
    void addPort(Connection* port);
    void printResults() const;
    size_t getPortsNum() const;

    std::map<std::pair<int, int>, double> getResults() const;
    int getIndex() const;
    void printEnergy(std::ostream& out) const;

    // True (measured) global accumulation count across all reduction units, for the
    // energy/cycle breakdown. Incremented on each psum add in the real datapath.
    static void     resetAccumulation();
    static uint64_t accumulationCount();
    //std::map<int, std::vector<std::tuple<double, int, int>>> getResults() const;
};

#endif
