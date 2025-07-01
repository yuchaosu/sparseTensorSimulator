#ifndef GRID_UPDATE_H
#define GRID_UPDATE_H

#include <vector>
#include "PE.h"
#include "Connection.h"
#include "DiagonalReduction.h"

class Grid {
public:
    Grid(int rows, int cols, std::vector<DiagonalReduction*>& diagonalReductions, std::vector<std::vector<int>> reductionMap, std::ostream& out);
    ~Grid();

    void setInputConnections(std::vector<Connection*> top_connections, 
                             std::vector<Connection*> left_connections);
    void setOutputConnections(std::vector<Connection*> output_connections);
    void setDiagonalReductions();
    void setInjectionFinished(bool finished); // Set injection finished status
    std::map<int, std::vector<std::tuple<double, int, int>>> getResults(); // Get results from diagonal reductions

    void cycle();  // Simulate one clock cycle

    PE* getPE(int row, int col) const;

    bool isIdle() const;  // Check if the grid is idle


private:
    int rows, cols;
    std::vector<std::vector<PE*>> pes;
    void connectNeighbors();  // << New helper function
    std::ostream& out;
    std::vector<DiagonalReduction*> diagonalReductions; // Store diagonal reductions
    std::vector<std::vector<int>> reductionMap;  // Store all connections for cycling
    std::map<int, std::vector<std::tuple<double, int, int>>> diagonals;
    bool injectionFinished; // Indicates if all data has been injected
    bool idle = true; // Indicates if the grid is idle
};

#endif // GRID_H
