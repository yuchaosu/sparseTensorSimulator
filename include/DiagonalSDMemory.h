#ifndef __DIAGONALSDMEMORY__H__
#define __DIAGONALSDMEMORY__H__

#include <list>
#include <map>
#include <vector>
#include "Tile.h"
#include "Connection.h"
#include "Fifo.h"
#include "types.h"
#include "DNNLayer.h"
#include "Unit.h"
#include "Config.h"
#include "DataPackage.h"
#include "Stats.h"
#include "MemoryController.h"
#include "MultiplierNetwork.h"
#include "ReduceNetwork.h"

// State machine for the diagonal dataflow controller
enum DiagonalControllerState {
    DIAG_CONFIGURING,       // Initial state for configuration
    DIAG_DIST_INPUTS,       // Distributing inputs according to diagonal schedule
    DIAG_WAITING_FOR_NEXT_ITER, // Waiting for the current iteration to complete
    DIAG_ALL_DATA_SENT      // All data has been sent
};

class DiagonalSDMemory : public MemoryController {
private:
    DNNLayer* dnn_layer;                // Layer loaded in the accelerator
    ReduceNetwork* reduce_network;      // Reduce network used to be reconfigured
    MultiplierNetwork* multiplier_network; // Multiplier network used to be reconfigured

    unsigned int M;                     // Number of rows in matrix A
    unsigned int N;                     // Number of columns in matrix B
    unsigned int K;                     // Common dimension (columns of A, rows of B)

    Connection* write_connection;
    DiagonalControllerState current_state; // Current state of the controller
    std::vector<unsigned int> vnat_table;  // VN address table

    std::vector<Connection*> read_connections; // Input port connections

    // Input parameters
    unsigned int ms_rows;               // Number of rows in the multiplier array
    unsigned int ms_cols;               // Number of columns in the multiplier array
    unsigned int n_read_ports;          // Number of read ports
    unsigned int n_write_ports;         // Number of write ports
    unsigned int write_buffer_capacity; // Capacity of the write buffer
    unsigned int port_width;            // Width of the ports

    unsigned int rows_used;             // Actual rows used in the current iteration
    unsigned int cols_used;             // Actual columns used in the current iteration

    unsigned int ms_size_per_input_port;

    // Fifos
    Fifo* write_fifo;                   // Fifo for write operations
    std::vector<Fifo*> input_fifos;     // Fifos for input operations
    std::vector<Fifo*> psum_fifos;      // Fifos for partial sum operations

    // Addresses
    address_t MK_address;               // Address of matrix A (MK)
    address_t KN_address;               // Address of matrix B (KN)
    address_t output_address;           // Address of output matrix

    // Diagonal dataflow specific parameters
    std::vector<int> A_offsets;         // Diagonal offsets for matrix A
    std::vector<int> B_offsets;         // Diagonal offsets for matrix B
    std::map<int, unsigned int> g_times; // Start times for A diagonals
    std::map<int, unsigned int> h_times; // Start times for B diagonals
    std::map<int, std::vector<data_t>> A_diag_vals; // Values for each A diagonal
    std::map<int, std::vector<data_t>> B_diag_vals; // Values for each B diagonal
    std::map<int, unsigned int> next_index_A; // Next index to inject for each A diagonal
    std::map<int, unsigned int> next_index_B; // Next index to inject for each B diagonal
    std::map<int, unsigned int> row_index;    // Map from A offset to PE row
    std::map<int, unsigned int> col_index;    // Map from B offset to PE column

    // Tile parameters
    unsigned int T_N;                   // Tile size for N dimension
    unsigned int T_K;                   // Tile size for K dimension
    unsigned int T_M;                   // Tile size for M dimension
    unsigned int iter_N;                // Number of iterations in N dimension
    unsigned int iter_K;                // Number of iterations in K dimension
    unsigned int iter_M;                // Number of iterations in M dimension

    // Current parameters
    unsigned int current_M;
    unsigned int current_N;
    unsigned int current_K;

    // Signals
    bool configuration_done;            // Indicates if configuration is done
    bool execution_finished;            // Indicates if execution is finished
    bool iteration_completed;           // Indicates if current iteration is completed
    bool metadata_loaded;               // Indicates if metadata is loaded
    bool layer_loaded;                  // Indicates if layer is loaded

    unsigned int current_output;        // Current output index
    unsigned int output_size;           // Total output size
    unsigned int current_output_iteration; // Current output iteration
    unsigned int n_iterations_completed;   // Number of completed iterations
    unsigned int output_size_iteration;    // Output size per iteration
    unsigned int max_cycles;            // Maximum number of cycles

    // For stats
    std::vector<Connection*> write_port_connections;
    cycles_t local_cycle;
    SDMemoryStats sdmemoryStats;        // To track information

    // Helper functions
    void receive();
    void send();
    void sendPackageToInputFifos(DataPackage* pck);
    std::vector<Connection*> getWritePortConnections() const { return this->write_port_connections; }
    
    // Diagonal dataflow specific functions
    void extractDiagonalValues();
    void scheduleDiagonalStreams();
    void configureSignals();

public:
    DiagonalSDMemory(id_t id, std::string name, Config stonne_cfg, Connection* write_connection);
    ~DiagonalSDMemory();
    
    void setLayer(DNNLayer* dnn_layer, address_t KN_address, address_t MK_address, address_t output_address, Dataflow dataflow);
    void setTile(Tile* current_tile);
    void setReadConnections(std::vector<Connection*> read_connections);
    void setWriteConnections(std::vector<Connection*> write_port_connections);
    void cycle();
    bool isExecutionFinished();

    void setDiagonalOffsets(const std::vector<int>& A_offsets, const std::vector<int>& B_offsets);
    
    void setSparseMatrixMetadata(metadata_address_t MK_metadata_id, metadata_address_t MK_metadata_pointer) { assert(false); } // Not supported
    void setDenseSpatialData(unsigned int T_N, unsigned int T_K) { assert(false); } // Not supported
    void setReduceNetwork(ReduceNetwork* reduce_network) { this->reduce_network = reduce_network; }
    void setMultiplierNetwork(MultiplierNetwork* multiplier_network) { this->multiplier_network = multiplier_network; }
    
    void printStats(std::ofstream& out, unsigned int indent);
    void printEnergy(std::ofstream& out, unsigned int indent);
};

#endif // __DIAGONALSDMEMORY__H__
