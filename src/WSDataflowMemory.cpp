#include "WSDataflowMemory.h"
#include "utility.h"
#include <iostream>

WSDataflowMemory::WSDataflowMemory(id_t id, std::string name, Config stonne_cfg) : MemoryController(id, name) {
    // Initialize configuration parameters
    this->n_rows = stonne_cfg.m_MSNetworkCfg.ms_size; // Or other parameter for PE grid height
    this->n_cols = stonne_cfg.m_MSNetworkCfg.ms_size; // Or other parameter for PE grid width
    
    // Determine the long side for memory connection
    this->long_side = (this->n_rows >= this->n_cols) ? this->n_rows : this->n_cols;
    
    // Configure connections and buffers
    // ...
}

void WSDataflowMemory::setMemoryConnections(Connection* write_connection) {
    // Connect to the long side of the grid
    // ...
}

void WSDataflowMemory::configureMemoryLanes() {
    // Configure lanes for both preload and compute data
    // ...
}

void WSDataflowMemory::sendPreloadData() {
    // Logic to send first batch of data (preload)
    // ...
}

void WSDataflowMemory::sendComputeData() {
    // Logic to send second batch of data (compute)
    // ...
}

void WSDataflowMemory::receiveOutputData() {
    // Logic to collect data from bottom row connections
    // ...
}

void WSDataflowMemory::cycle() {
    // Main cycle implementation
    // Phase tracking (preload vs compute)
    // Sending inputs and receiving outputs based on current phase
    // ...
    
    // Update stats
    this->sdmemoryStats.n_cycles++; 
}