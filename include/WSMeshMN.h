// WSMeshMN.h
#ifndef __WSMESHMN__H__
#define __WSMESHMN__H__

#include "MultiplierNetwork.h"
#include "Connection.h"
#include "MultiplierWS.h"
#include "Config.h"
#include "DNNLayer.h"
#include "Tile.h"
#include <map>

class WSMeshMN : public MultiplierNetwork {
private:
    std::map<std::pair<int, int>, MultiplierWS*> mswitchtable;
    std::map<std::pair<int, int>, Connection*> verticalconnectiontable;
    std::map<std::pair<int, int>, Connection*> horizontalconnectiontable;
    std::map<std::pair<int, int>, Connection*> diagonalconnectiontable;
    
    unsigned int n_rows;
    unsigned int n_cols;
    unsigned int long_side;
    unsigned int port_width;
    unsigned int buffers_capacity;
    
    void setPhysicalConnection(); // Create the mesh connections
    
public:
    WSMeshMN(id_t id, std::string name, Config stonne_cfg);
    ~WSMeshMN();
    
    // Interface methods
    void setInputConnections(std::map<int, Connection*> input_connections);
    void setOutputConnections(std::map<int, Connection*> output_connections);
    void cycle();
    
    // Configuring methods for your specific dataflow
    void configurePreloadPhase(Tile* current_tile, DNNLayer* dnn_layer);
    void configureComputePhase(Tile* current_tile, DNNLayer* dnn_layer);
    void resetSignals();
    
    // For debugging and statistics
    void printConfiguration(std::ofstream& out, unsigned int indent);
    void printStats(std::ofstream& out, unsigned int indent);
    void printEnergy(std::ofstream& out, unsigned int indent);
};

#endif