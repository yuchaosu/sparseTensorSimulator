#ifndef __MultiplierWS__H
#define __MultiplierWS__H

#include "types.h"
#include "DataPackage.h"
#include "Connection.h"
#include "Fifo.h"
#include "Unit.h"
#include <vector>
#include "Config.h"
#include "Stats.h"

class MultiplierWS : public Unit {
private:
    // Input/output FIFOs
    Fifo* top_fifo;         // Packages from top (weights)
    Fifo* left_fifo;        // Packages from left (activations)
    Fifo* right_fifo;       // Packages to send right (activations)
    Fifo* bottom_fifo;      // Packages to send bottom (weights)
    Fifo* accbuffer_fifo;   // Psum to be sent to parent
    
    // Diagonal FIFOs
    Fifo* top_left_fifo;    // Packages from top-left diagonal
    Fifo* bottom_right_fifo; // Packages to send bottom-right diagonal
    
    // Orthogonal connections
    Connection* left_connection;     // To left neighbor or memory
    Connection* right_connection;    // To right neighbor
    Connection* top_connection;      // To top neighbor or memory
    Connection* bottom_connection;   // To bottom neighbor
    Connection* accbuffer_connection; // To accumulation buffer
    
    // Diagonal connections 
    Connection* top_left_connection;    // From top-left neighbor
    Connection* bottom_right_connection; // To bottom-right neighbor
    
    // Position info
    int row_num;
    int col_num;
    int num;  // General identifier
    
    // Architecture parameters
    unsigned int input_ports;
    unsigned int output_ports;
    unsigned int forwarding_ports;
    unsigned int buffers_capacity;
    unsigned int port_width;
    unsigned int ms_rows;
    unsigned int ms_cols;
    cycles_t latency;
    cycles_t local_cycle;
    
    // Stats collection
    MultiplierOSStats mswitchStats;
    
    // Control signals
    unsigned int VN;
    bool forward_right;         // Forward to right neighbor
    bool forward_bottom;        // Forward to bottom neighbor
    bool forward_diagonal;      // Forward diagonally (new)
    bool preload_phase;         // Indicates if in preload phase (new)
    bool compute_phase;         // Indicates if in compute phase (new)

public:
    // Constructor/Destructor
    MultiplierWS(id_t id, std::string name, int row_num, int col_num, Config stonne_cfg);
    MultiplierWS(id_t id, std::string name, int row_num, int col_num, Config stonne_cfg, 
                Connection* left_connection, Connection* right_connection, 
                Connection* top_connection, Connection* bottom_connection,
                Connection* top_left_connection, Connection* bottom_right_connection);
    ~MultiplierWS();
    
    // Connection setters
    void setTopConnection(Connection* top_connection);
    void setLeftConnection(Connection* left_connection);
    void setRightConnection(Connection* right_connection);
    void setBottomConnection(Connection* bottom_connection);
    void setAccBufferConnection(Connection* accbuffer_connection);
    void setTopLeftConnection(Connection* top_left_connection);
    void setBottomRightConnection(Connection* bottom_right_connection);
    
    // Data flow operations
    void send();           // Send data to neighbors
    void receive();        // Receive data from neighbors
    DataPackage* perform_operation_2_operands(DataPackage* pck_left, DataPackage* pck_right);
    
    // WS dataflow specific operations
    void preloadOperation(); // Handle preload phase operations
    void computeOperation(); // Handle compute phase operations
    
    // Control methods
    void cycle();          // Process one cycle
    void resetSignals();   // Reset all signals
    
    // Signal configuration
    void configureBottomSignal(bool bottom_signal);
    void configureRightSignal(bool right_signal);
    void configureDiagonalSignal(bool diagonal_signal);
    void setPreloadPhase(bool preload);
    void setComputePhase(bool compute);
    void setVirtualNeuron(unsigned int VN);
    
    // Debug and statistics
    void printConfiguration(std::ofstream& out, unsigned int indent);
    void printStats(std::ofstream& out, unsigned int indent);
    void printEnergy(std::ofstream& out, unsigned int indent);
};

#endif
