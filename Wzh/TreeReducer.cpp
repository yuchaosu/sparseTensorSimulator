#include "TreeReducer.h"
#include <iostream>

void TreeReducer::cycle() {
    for (auto* conn : bottomPorts) {
        if (conn->pendingPsum()) {
            DataPackage psum = conn->sendPsum();
            psumOut[{psum.index1, psum.index2}] += psum.value;
        }
        if (conn->pendingTransfer()) {
            DataPackage trans = conn->sendTransfer();
            psumOut[{trans.index1, trans.index2}] += trans.value;
        }
    }
}

void TreeReducer::printResults() const {
    for (const auto& entry : psumOut) {
        auto [i, j] = entry.first;
        std::cout << "C[" << i << "][" << j << "] = " << entry.second << std::endl;
    }
}
