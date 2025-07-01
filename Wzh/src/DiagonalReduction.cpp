#include "../include/DiagonalReduction.h"
#include <iostream>

DiagonalReduction::DiagonalReduction(int index, std::ostream& output_stream) : index(index), out(output_stream) {}

void DiagonalReduction::cycle() {
    for (auto* conn : diagonalPorts) {
        if (conn == nullptr) {
        std::cerr << "[Error] Null connection pointer in diagonalPorts.\n";
        continue;
        }
        if (conn->pendingPsum()) {
            DataPackage psum = conn->sendPsum();
            std::pair<int, int> key = {psum.index1, psum.index2};
            diagonal[key] += psum.value;
            out << "DiagonalReduction " << index << " received psum: " << psum.value 
                << " at index (" << psum.index1 << ", " << psum.index2 << ") -> Total: " 
                << diagonal[key] << "\n";
        }
    }
}

std::map<std::pair<int, int>, double> DiagonalReduction::getResults() const {
    return diagonal;
}

int DiagonalReduction::getIndex() const {
    return index;
}

void DiagonalReduction::addPort(Connection* port) {
    diagonalPorts.push_back(port);
}

size_t DiagonalReduction::getPortsNum() const {
    return diagonalPorts.size();
}