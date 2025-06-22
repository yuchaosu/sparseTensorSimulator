#include "../include/Grid_Update.h"
#include <cassert>
#include <iostream>

Grid::Grid(int rows, int cols, std::vector<DiagonalReduction*>& diagonal_reductions, std::vector<std::vector<int>> reduction_map, std::ostream& output_stream) : 
            rows(rows), cols(cols), out(output_stream), diagonalReductions(diagonal_reductions), reductionMap(reduction_map) {
    pes.resize(rows, std::vector<PE*>(cols, nullptr));

    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            pes[i][j] = new PE(i, j, out);
            if (i == rows - 1) {
                pes[i][j]->setLastRow(true);  // Set last row flag for the last row PEs
            }
            if (j == cols - 1) {
                pes[i][j]->setLastCol(true);  // Set last column flag for the last column PEs
            }
        }


    connectNeighbors();  // << delegate neighbor wiring here
    out << "Reduction Map Size in Grid: " << reductionMap.size() << std::endl;
}

Grid::~Grid() {
    for (auto& row : pes)
        for (auto pe : row)
            delete pe;
}


void Grid::connectNeighbors() {
    // Phase 1: Connect neighbor PEs vertically and horizontally
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            // Vertical connection: from PE(i-1, j) bottom to PE(i, j) top
            if (i > 0) {
                Connection* vertical_conn = new Connection(out);
                pes[i][j]->setTopConnection(vertical_conn);
                pes[i - 1][j]->setBottomConnection(vertical_conn);
            }

            // Horizontal connection: from PE(i, j-1) right to PE(i, j) left
            if (j > 0) {
                Connection* horizontal_conn = new Connection(out);
                pes[i][j]->setLeftConnection(horizontal_conn);
                pes[i][j - 1]->setRightConnection(horizontal_conn);
            }
        }
    }

    // Phase 2: Connect to Diagonal Reductions
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            // Ensure edge PE (typically last row) has a bottom connection
            if (pes[i][j]->getBottomConnection() == nullptr) {
                Connection* vertical_conn = new Connection(out);
                pes[i][j]->setBottomConnection(vertical_conn);
            }

            Connection* bottom_conn = pes[i][j]->getBottomConnection();

            // Only connect to the appropriate DiagonalReduction
            if (bottom_conn != nullptr) {
                for (auto& reduction : diagonalReductions) {
                    if (reduction->getIndex() == reductionMap[i][j]) {
                        reduction->addPort(bottom_conn);
                        out << "PE (" << i << ", " << j << ") connected to Diagonal Reduction " << reduction->getIndex() << std::endl;
                        out << "Reduction at " << reduction->getIndex() << " has " << reduction->getPortsNum() << " ports." << std::endl;
                        break;  // Only one matching reduction per PE
                    }
                }
            }
            else {
                out << "Warning: PE (" << i << ", " << j << ") has no bottom connection to a Diagonal Reduction." << std::endl;
            }
        }
    }
}


void Grid::setInputConnections(std::vector<Connection*> top_connections, 
                              std::vector<Connection*> left_connections) {
    assert(top_connections.size() == static_cast<size_t>(cols));
    assert(left_connections.size() == static_cast<size_t>(rows));

    for (int j = 0; j < cols; ++j)
        pes[0][j]->setTopConnection(top_connections[j]);

    for (int i = 0; i < rows; ++i)
        pes[i][0]->setLeftConnection(left_connections[i]);
}

void Grid::setOutputConnections(std::vector<Connection*> output_connections) {
    assert(output_connections.size() == static_cast<size_t>(cols));

    for (int j = 0; j < cols; ++j)
        pes[rows - 1][j]->setBottomConnection(output_connections[j]);
}

void Grid::cycle() {
    for (int i = 0; i < rows; ++i){
        for (int j = 0; j < cols; ++j) {
            pes[i][j]->cycle();
            idle = true;
            out << "PE (" << i << ", " << j << ") state after cycle: "
                << (pes[i][j]->isIdle()? "Idle" : "Active") << std::endl; // Log idle state of each PE
            //out << "Grid State: " << (idle ? "Idle" : "Active") << std::endl;
            idle = idle && pes[i][j]->isIdle(); // Update idle state based on PEs
            out << "Grid State: " << (idle ? "Idle" : "Active") << std::endl;
            //pes[i][j]->setInjectionFinished(injectionFinished);  // Set injection finished status for each PE
        }
    }

    for(auto& reduction : diagonalReductions) {

        reduction->cycle();  // Cycle through each diagonal reduction
    }
}

PE* Grid::getPE(int row, int col) const {
    return pes[row][col];
}

bool Grid::isIdle() const {
    return idle;
}

std::map<int, std::vector<std::tuple<double, int, int>>> Grid::getResults() {
    std::map<int, std::vector<std::tuple<double, int, int>>> results;
    for (const auto& reduction : diagonalReductions) {
        std::map<std::pair<int, int>, double> diag_results = reduction->getResults();
        for (const auto& [key, value] : diag_results) {
            results[reduction->getIndex()].emplace_back(value, key.first, key.second);
        }
    }
    return results;
}

void Grid::setInjectionFinished(bool finished) {
    injectionFinished = finished;
}
