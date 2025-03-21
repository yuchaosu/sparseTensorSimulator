#ifndef __WSDATAFLOWMEMORY__H__
#define __WSDATAFLOWMEMORY__H__

#include "MemoryController.h"
#include "Connection.h"
#include "Config.h"
#include "Stats.h"

class WSDataflowMemory : public MemoryController {
private:
    // PE grid dimensions
    unsigned int n_rows;
    unsigned int n_cols;
    unsigned int long_side;
    
    // Dataflow state tracking
    bool preload_phase_complete;
    
    // Connections
    Connection* memory_connections;
    
    // Stats collection
    SDMemoryStats sdmemoryStats;

public:
    WSDataflowMemory(id_t id, std::string name, Config stonne_cfg);
    void setMemoryConnections(Connection* write_connection);
    void configureMemoryLanes();
    void sendPreloadData();
    void sendComputeData();
    void receiveOutputData();
    void cycle();
    
    // Other necessary methods...
};

#endif 