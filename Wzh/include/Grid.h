#ifndef GRID_H
#define GRID_H

#include <vector>
#include "PE.h"
#include "Connection.h"

class Grid {
public:
    Grid(int rows, int cols);
    ~Grid();

    void setInputConnections(std::vector<Connection*> top_connections, 
                             std::vector<Connection*> left_connections);
    void setOutputConnections(std::vector<Connection*> output_connections);
    void cycle();  // Simulate one clock cycle

    PE* getPE(int row, int col) const;

    bool isIdle() const;  // Check if the grid is idle


private:
    int rows, cols;
    std::vector<std::vector<PE*>> pes;
    std::vector<Connection*> connections;  // Store all connections for cycling
    void connectNeighbors();  // << New helper function
    //bool idle = false; // Indicates if the grid is idle
};

#endif // GRID_H
