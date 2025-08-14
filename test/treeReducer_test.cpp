#include "TreeReducer.h"
#include "Connection.h"
#include <iostream>
#include <vector>
#include <cassert>

int main() {
    const int num_outputs = 3;
    std::vector<Connection*> bottom_ports(num_outputs);
    for (int i = 0; i < num_outputs; ++i)
        bottom_ports[i] = new Connection();

    TreeReducer reducer(bottom_ports);

    // ==== Cycle 0 ====
    bottom_ports[0]->receivePsum(DataPackage(4, 0, 0));
    bottom_ports[1]->receivePsum(DataPackage(5, 0, 1));
    bottom_ports[2]->receivePsum(DataPackage(6, 0, 2));
    reducer.cycle();  // cycle 0

    // ==== Cycle 1 ====
    bottom_ports[0]->receiveTransfer(DataPackage(2, 0, 0));
    bottom_ports[1]->receiveTransfer(DataPackage(3, 0, 1));
    bottom_ports[2]->receiveTransfer(DataPackage(1, 0, 2));
    reducer.cycle();  // cycle 1

    std::cout << "Final Results:\n";
    reducer.printResults();

    // Verification
    auto results = reducer.getResults();
    assert(results[std::make_pair(0, 0)] == 6);
    assert(results[std::make_pair(0, 1)] == 8);
    assert(results[std::make_pair(0, 2)] == 7);
    std::cout << "Test Passed!" << std::endl;

    for (auto conn : bottom_ports)
        delete conn;

    return 0;
}
