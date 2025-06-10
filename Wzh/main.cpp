#include "Grid.h"
#include "TreeReducer.h"
#include <vector>
#include <map>
#include <iostream>
#include "Utility.h"

int main() {
    const int SIZE = 3;

    Grid grid(SIZE, SIZE);

    // Setup input connections
    std::vector<Connection*> left_in(SIZE), top_in(SIZE);
    for (int i = 0; i < SIZE; ++i) {
        left_in[i] = new Connection();
        top_in[i] = new Connection();
    }
    grid.setInputConnections(top_in, left_in);

    // Setup output connections
    std::vector<Connection*> bottom_out(SIZE);
    for (int i = 0; i < SIZE; ++i)
        bottom_out[i] = new Connection();
    grid.setOutputConnections(bottom_out);

    // Setup TreeReducer for collecting psum + transfer
    TreeReducer reducer(bottom_out);

    // Define column injections
    std::map<int, std::vector<DataPackage>> col_inputs = {
        {0, {{1,2,0}, {2,3,1}, {3,4,2}}},
        {1, {{4,0,0}, {5,1,1}, {6,2,2}, {7,3,3}, {8,4,4}}},
        {2, {{10,0,2}, {11,1,3}, {12,2,4}}}
    };

    // Define row injections
    std::map<int, std::vector<DataPackage>> row_inputs = {
        {0, {{10,0,2}, {11,1,3}, {12,2,4}}},
        {1, {{4,0,0}, {5,1,1}, {6,2,2}, {7,3,3}, {8,4,4}}},
        {2, {{1,2,0}, {2,3,1}, {3,4,2}}} // <- previously missing
    };

    // Run simulation
    const int MAX_CYCLES = 15;
    for (int cycle = 0; cycle < MAX_CYCLES; ++cycle) {
        std::cout << "===== Cycle " << cycle << " =====\n";

        // Inject column data (A input)
        for (int col = 0; col < SIZE; ++col) {
            if (col_inputs.count(col) && cycle - col >= 0) {
                const auto& vec = col_inputs[col];
                if (cycle - col < vec.size()) {
                    if (!top_in[col]->pendingSrc())
                        top_in[col]->receiveSrc(vec[cycle - col]);
                }
            }
        }

        // Inject row data (B input)
        for (int row = 0; row < SIZE; ++row) {
            if (row_inputs.count(row) && cycle - row >= 0) {
                const auto& vec = row_inputs[row];
                if (cycle - row < vec.size()) {
                    if (!left_in[row]->pendingSrc())
                        left_in[row]->receiveSrc(vec[cycle - row]);
                }
            }
        }

        grid.cycle();
        reducer.cycle();
    }

    reducer.printResults();
    return 0;
}
