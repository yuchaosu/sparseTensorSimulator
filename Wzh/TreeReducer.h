#ifndef TREEREDUCER_H
#define TREEREDUCER_H

#include "Connection.h"
#include <map>
#include <vector>

class TreeReducer {
private:
    std::vector<Connection*> bottomPorts;
    std::map<std::pair<int, int>, int> psumOut;

public:
    TreeReducer(const std::vector<Connection*>& ports) : bottomPorts(ports) {}

    void cycle();  // collect one cycle worth of data

    void printResults() const;
    std::map<std::pair<int, int>, int> getResults() const { return psumOut; }
};

#endif
