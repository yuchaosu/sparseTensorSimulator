#include "WSMeshMN.h"
#include "utility.h"
#include <assert.h>

WSMeshMN::WSMeshMN(id_t id, std::string name, Config stonne_cfg) : MultiplierNetwork(id, name) {
    // Configuration parameters
    this->n_rows = stonne_cfg.m_MSNetworkCfg.ms_rows;
    this->n_cols = stonne_cfg.m_MSNetworkCfg.ms_cols;
    this->port_width = stonne_cfg.m_MSwitchCfg.port_width;
    this->buffers_capacity = stonne_cfg.m_MSwitchCfg.buffers_capacity;
    
    // Determine the long side for memory connection
    this->long_side = (this->n_rows >= this->n_cols) ? this->n_rows : this->n_cols;
    
    // Create the grid of multipliers
    for(int i=0; i < this->n_rows; i++) {
        for(int j=0; j < this->n_cols; j++) {
            std::string ms_str = "MultiplierWS " + std::to_string(i) + ":" + std::to_string(j);
            unsigned int ms_id = i * this->n_cols + j;
            MultiplierWS* ms = new MultiplierWS(ms_id, ms_str, i, j, stonne_cfg);
            std::pair<int, int> rowandcolumn(i, j);
            mswitchtable[rowandcolumn] = ms;
        }
    }
    
    // Create connections between multipliers
    setPhysicalConnection();
}

WSMeshMN::~WSMeshMN() {
    // Free vertical connections
    for(auto& conn : verticalconnectiontable) {
        delete conn.second;
    }
    
    // Free horizontal connections
    for(auto& conn : horizontalconnectiontable) {
        delete conn.second;
    }

    // Free diagonal connections
    for(auto& conn : diagonalconnectiontable) {
        delete conn.second;
    }
    
    // Free multipliers
    for(auto& ms : mswitchtable) {
        delete ms.second;
    }
}

// Create the mesh connections (similar to OSMeshMN)
void WSMeshMN::setPhysicalConnection() {
    for(int i=0; i < this->n_rows; i++) {
        for(int j=0; j < this->n_cols; j++) {
            std::pair<int, int> rowandcolumn(i, j);
            MultiplierWS* ms = mswitchtable[rowandcolumn];
            
            // Vertical connections (top-bottom)
            if(i > 0) {
                Connection* vertical_conn = new Connection(port_width);
                verticalconnectiontable[rowandcolumn] = vertical_conn;
                ms->setTopConnection(vertical_conn);
                std::pair<int, int> mstop_index(i-1, j);
                MultiplierWS* mstop = mswitchtable[mstop_index];
                mstop->setBottomConnection(vertical_conn);
            }
            
            // Horizontal connections (left-right)
            if(j > 0) {
                Connection* horizontal_conn = new Connection(port_width);
                horizontalconnectiontable[rowandcolumn] = horizontal_conn;
                ms->setLeftConnection(horizontal_conn);
                std::pair<int, int> msleft_index(i, j-1);
                MultiplierWS* msleft = mswitchtable[msleft_index];
                msleft->setRightConnection(horizontal_conn);
            }

            //Diagonal connections (top-left to bottom-right)
            if(i > 0 && j > 0) {
                Connection* diagonal_conn = new Connection(port_width);
                diagonalconnectiontable[rowandcolumn] = diagonal_conn;
                ms->setTopLeftConnection(diagonal_conn);
                std::pair<int, int> mstopleft_index(i-1, j-1);
                MultiplierWS* mstopleft = mswitchtable[mstopleft_index];
                mstopleft->setBottomRightConnection(diagonal_conn);
            }
        }
    }
}

// Connect inputs to the long side
void WSMeshMN::setInputConnections(std::map<int, Connection*> input_connections) {
    // If rows >= cols, connect to leftmost column
    if(n_rows >= n_cols) {
        for(int i=0; i < n_rows; i++) {
            if(input_connections.find(i) != input_connections.end()) {
                std::pair<int, int> ms_index(i, 0);
                MultiplierWS* ms = mswitchtable[ms_index];
                ms->setLeftConnection(input_connections[i]);
            }
        }
    }
    // Otherwise connect to top row
    else {
        for(int j=0; j < n_cols; j++) {
            if(input_connections.find(j) != input_connections.end()) {
                std::pair<int, int> ms_index(0, j);
                MultiplierWS* ms = mswitchtable[ms_index];
                ms->setTopConnection(input_connections[j]);
            }
        }
    }
}

// Connect outputs to the bottom row
void WSMeshMN::setOutputConnections(std::map<int, Connection*> output_connections) {
    for(int j=0; j < n_cols; j++) {
        if(output_connections.find(j) != output_connections.end()) {
            std::pair<int, int> ms_index(n_rows-1, j);
            MultiplierWS* ms = mswitchtable[ms_index];
            ms->setBottomConnection(output_connections[j]);
        }
    }
}

void WSMeshMN::cycle() {
    // Cycle through all multipliers in the grid
    for(auto& ms_entry : mswitchtable) {
        ms_entry.second->cycle();
    }
}

void WSMeshMN::configurePreloadPhase(Tile* current_tile, DNNLayer* dnn_layer) {
    // Configure multipliers for preload phase
    // Set forwarding signals appropriately
    for(int i=0; i < n_rows; i++) {
        for(int j=0; j < n_cols; j++) {
            std::pair<int, int> ms_index(i, j);
            MultiplierWS* ms = mswitchtable[ms_index];
            
            // Configure preload phase signals
            // (You'll need to implement your specific preload logic)
            
            // Example: Enable right forwarding for left-to-right flow
            ms->configureRightSignal(j < n_cols-1);
            
            // Example: Enable bottom forwarding for top-to-bottom flow
            ms->configureBottomSignal(i < n_rows-1);
            
            ms->setPreloadPhase(true);
            ms->setComputePhase(false);
        }
    }
}

void WSMeshMN::configureComputePhase(Tile* current_tile, DNNLayer* dnn_layer) {
    // Configure multipliers for compute phase
    // Set forwarding signals appropriately
    for(int i=0; i < n_rows; i++) {
        for(int j=0; j < n_cols; j++) {
            std::pair<int, int> ms_index(i, j);
            MultiplierWS* ms = mswitchtable[ms_index];
            
            // Configure compute phase signals
            // (You'll need to implement your specific compute logic)
            
            // Example: Enable specific dataflow patterns for the compute phase
            
            ms->setPreloadPhase(false);
            ms->setComputePhase(true);
        }
    }
}

void WSMeshMN::resetSignals() {
    for(int i=0; i < n_rows; i++) {
        for(int j=0; j < n_cols; j++) {
            std::pair<int, int> ms_index(i, j);
            MultiplierWS* ms = mswitchtable[ms_index];
            ms->configureBottomSignal(false);
            ms->configureRightSignal(false);
            ms->setVirtualNeuron(0);
        }
    }
}

// Implementation of other methods for stats and configuration...
void WSMeshMN::printConfiguration(std::ofstream& out, unsigned int indent) {
    // Print configuration info
}

void WSMeshMN::printStats(std::ofstream& out, unsigned int indent) {
    // Print stats
}

void WSMeshMN::printEnergy(std::ofstream& out, unsigned int indent) {
    // Print energy usage
}
