#include "Grid.h"
#include <cassert>
#include <iostream>

Grid::Grid(int rows, int cols) : rows(rows), cols(cols) {
    pes.resize(rows, std::vector<PE*>(cols, nullptr));

    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
            pes[i][j] = new PE(i, j);

    connectNeighbors();  // << delegate neighbor wiring here
}

Grid::~Grid() {
    for (auto& row : pes)
        for (auto pe : row)
            delete pe;
}


void Grid::connectNeighbors() {
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            if (i > 0) {
                Connection* vertical_conn = new Connection();
                pes[i][j]->setTopConnection(vertical_conn);
                pes[i - 1][j]->setBottomConnection(vertical_conn);  // Connect top PE's bottom to current PE's top
            }
            if (j > 0) {
                Connection* horizontal_conn = new Connection();
                pes[i][j]->setLeftConnection(horizontal_conn);
                pes[i][j - 1]->setRightConnection(horizontal_conn);  // Connect left PE's right to current PE's left
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
    for(auto& conn : connections) {
        conn->cycle();
    }
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
            pes[i][j]->cycle();
}

PE* Grid::getPE(int row, int col) const {
    return pes[row][col];
}
