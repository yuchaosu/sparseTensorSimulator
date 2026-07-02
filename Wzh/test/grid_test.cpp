#include "Grid.h"
#include "Connection.h"
#include "Utility.h"
#include <iostream>
#include <vector>
#include <cassert>

void injectInputs(std::vector<Connection*>& top, std::vector<Connection*>& left) {
    // B values from top: B[0][0], B[0][1], B[0][2]
    top[0]->receiveSrc(DataPackage(1.0, 0, 0));  // index1 = 0
    top[1]->receiveSrc(DataPackage(2.0, 1, 1));  // index1 = 1
    top[2]->receiveSrc(DataPackage(3.0, 2, 2));  // index1 = 2

    // A values from left: A[0][0], A[1][1], A[2][2]
    left[0]->receiveSrc(DataPackage(4.0, 0, 0));  // index2 = 0
    left[1]->receiveSrc(DataPackage(5.0, 1, 1));  // index2 = 1
    left[2]->receiveSrc(DataPackage(6.0, 2, 2));  // index2 = 2
}

void runCycles(Grid& grid, int count) {
    for (int i = 0; i < count; ++i) {
        std::cout << "===== Cycle " << i << " =====\n";
    grid.cycle(cycle);
    }
}

void checkOutputs(std::vector<Connection*>& bottom) {
    std::vector<float> expected = {4.0, 10.0, 18.0}; // 1×4, 2×5, 3×6

    for (int j = 0; j < bottom.size(); ++j) {
        bool received = false;
        DataPackage result;

        // First check for Psum output
        if (bottom[j]->pendingPsum()) {
            result = bottom[j]->sendPsum();
            received = true;
            std::cout << "✅ Bottom[" << j << "] received Psum: " << result.value << "\n";
        }
        // Otherwise check for Transfer output
        else if (bottom[j]->pendingTransfer()) {
            result = bottom[j]->sendTransfer();
            received = true;
            std::cout << "✅ Bottom[" << j << "] received Transfer: " << result.value << "\n";
        }

        if (received) {
            assert(result.value == expected[j]);
            assert(result.index1 == j);
            assert(result.index2 == j);
        } else {
            std::cerr << "❌ Bottom[" << j << "] received nothing\n";
            assert(false && "Expected result not received at this output port.");
        }
    }

    std::cout << "✅ All outputs are correct.\n";
}

int main() {
    const int rows = 3, cols = 3;
    Grid grid(rows, cols);

    // Create connections
    std::vector<Connection*> top(cols), left(rows), bottom(cols);
    for (int i = 0; i < cols; ++i) top[i] = new Connection();
    for (int i = 0; i < rows; ++i) left[i] = new Connection();
    for (int i = 0; i < cols; ++i) bottom[i] = new Connection();

    // Attach connections
    grid.setInputConnections(top, left);
    grid.setOutputConnections(bottom);

    // Inject input values
    injectInputs(top, left);

    // Run enough cycles for full propagation and compute
    runCycles(grid, 6);

    // Validate bottom outputs
    checkOutputs(bottom);

    // Cleanup
    for (auto conn : top) delete conn;
    for (auto conn : left) delete conn;
    for (auto conn : bottom) delete conn;

    return 0;
}
