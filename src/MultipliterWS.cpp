// MultiplierWS.cpp
#include "MultiplierWS.h"
#include <assert.h>
#include "utility.h"

MultiplierWS::MultiplierWS(id_t id, std::string name, int row_num, int col_num, Config stonne_cfg) : Unit(id, name) {
    // Initialize position
    this->row_num = row_num;
    this->col_num = col_num;
    this->num = row_num * stonne_cfg.m_MSNetworkCfg.ms_cols + col_num;
    
    // Initialize architecture parameters
    this->input_ports = stonne_cfg.m_MSwitchCfg.input_ports;
    this->output_ports = stonne_cfg.m_MSwitchCfg.output_ports;
    this->forwarding_ports = stonne_cfg.m_MSwitchCfg.forwarding_ports;
    this->buffers_capacity = stonne_cfg.m_MSwitchCfg.buffers_capacity;
    this->port_width = stonne_cfg.m_MSwitchCfg.port_width;
    this->ms_rows = stonne_cfg.m_MSNetworkCfg.ms_rows;
    this->ms_cols = stonne_cfg.m_MSNetworkCfg.ms_cols;
    this->latency = stonne_cfg.m_MSwitchCfg.latency;
    
    // Create FIFOs
    this->top_fifo = new Fifo(this->buffers_capacity);
    this->left_fifo = new Fifo(this->buffers_capacity);
    this->right_fifo = new Fifo(this->buffers_capacity);
    this->bottom_fifo = new Fifo(this->buffers_capacity);
    this->accbuffer_fifo = new Fifo(this->buffers_capacity);
    this->top_left_fifo = new Fifo(this->buffers_capacity);
    this->bottom_right_fifo = new Fifo(this->buffers_capacity);
    
    // Initialize connections
    this->top_connection = NULL;
    this->left_connection = NULL;
    this->bottom_connection = NULL;
    this->right_connection = NULL;
    this->accbuffer_connection = NULL;
    this->top_left_connection = NULL;
    this->bottom_right_connection = NULL;
    
    // Initialize control signals
    this->forward_right = false;
    this->forward_bottom = false;
    this->forward_diagonal = false;
    this->preload_phase = false;
    this->compute_phase = false;
    this->VN = 0;
    this->local_cycle = 0;
}

MultiplierWS::~MultiplierWS() {
    // Clean up FIFOs
    delete this->top_fifo;
    delete this->left_fifo;
    delete this->right_fifo;
    delete this->bottom_fifo;
    delete this->accbuffer_fifo;
    delete this->top_left_fifo;
    delete this->bottom_right_fifo;
}

void MultiplierWS::setTopConnection(Connection* top_connection) {
    this->top_connection = top_connection;
}

void MultiplierWS::setLeftConnection(Connection* left_connection) {
    this->left_connection = left_connection;
}

void MultiplierWS::setRightConnection(Connection* right_connection) {
    this->right_connection = right_connection;
}

void MultiplierWS::setBottomConnection(Connection* bottom_connection) {
    this->bottom_connection = bottom_connection;
}

void MultiplierWS::setAccBufferConnection(Connection* accbuffer_connection) {
    this->accbuffer_connection = accbuffer_connection;
}

void MultiplierWS::setTopLeftConnection(Connection* top_left_connection) {
    this->top_left_connection = top_left_connection;
}

void MultiplierWS::setBottomRightConnection(Connection* bottom_right_connection) {
    this->bottom_right_connection = bottom_right_connection;
}

void MultiplierWS::send() {
    // Process and forward data based on the current phase and signals
    
    // Send right (Data1)
    std::vector<DataPackage*> vector_to_send_right;
    while(!this->right_fifo->isEmpty()) {
        DataPackage* pck = this->right_fifo->pop();
        vector_to_send_right.push_back(pck);
    }
    if(this->right_connection && this->forward_right && vector_to_send_right.size() > 0) {
        this->right_connection->send(vector_to_send_right);
        // Track stats
        this->mswitchStats.n_right_forwardings_send++;
    }
    
    // Send bottom (Data2)
    std::vector<DataPackage*> vector_to_send_bottom;
    while(!this->bottom_fifo->isEmpty()) {
        DataPackage* pck = this->bottom_fifo->pop();
        vector_to_send_bottom.push_back(pck);
    }
    if(this->bottom_connection && this->forward_bottom && vector_to_send_bottom.size() > 0) {
        this->bottom_connection->send(vector_to_send_bottom);
        // Track stats
        this->mswitchStats.n_bottom_forwardings_send++;
    }
    
    // Send diagonal (Psum)
    std::vector<DataPackage*> vector_to_send_diagonal;
    while(!this->bottom_right_fifo->isEmpty()) {
        DataPackage* pck = this->bottom_right_fifo->pop();
        vector_to_send_diagonal.push_back(pck);
    }
    if(this->bottom_right_connection && this->forward_diagonal && vector_to_send_diagonal.size() > 0) {
        this->bottom_right_connection->send(vector_to_send_diagonal);
        // Would need to add stats tracking for diagonal sends
    }
    
    // Send to accumulation buffer
    std::vector<DataPackage*> vector_to_send_accbuffer;
    while(!this->accbuffer_fifo->isEmpty()) {
        DataPackage* pck = this->accbuffer_fifo->pop();
        vector_to_send_accbuffer.push_back(pck);
    }
    if(this->accbuffer_connection && vector_to_send_accbuffer.size() > 0) {
        this->accbuffer_connection->send(vector_to_send_accbuffer);
    }
}

void MultiplierWS::receive() {
    // Receive data from input connections
    
    // From top (weights)
    if(this->top_connection && this->top_connection->existPendingData()) {
        std::vector<DataPackage*> pck_vector = this->top_connection->receive();
        for(DataPackage* pck : pck_vector) {
            if(this->preload_phase) {
                // In preload phase, just store in buffer
                this->top_fifo->push(pck);
            }
            else if(this->compute_phase) {
                // In compute phase, either use for multiplication or forward
                if(this->forward_bottom) {
                    this->bottom_fifo->push(pck);
                }
                else {
                    // Use for local computation
                    // Process based on WSDataflow logic
                }
            }
            // Track stats
            this->mswitchStats.n_top_forwardings_receive++;
        }
    }
    
    // From left (activations)
    if(this->left_connection && this->left_connection->existPendingData()) {
        std::vector<DataPackage*> pck_vector = this->left_connection->receive();
        for(DataPackage* pck : pck_vector) {
            if(this->preload_phase) {
                // In preload phase, just store in buffer
                this->left_fifo->push(pck);
            }
            else if(this->compute_phase) {
                // In compute phase, either use for multiplication or forward
                if(this->forward_right) {
                    this->right_fifo->push(pck);
                }
                else {
                    // Use for local computation
                    // Process based on WSDataflow logic
                }
            }
            // Track stats
            this->mswitchStats.n_left_forwardings_receive++;
        }
    }
    
    // From top-left diagonal
    if(this->top_left_connection && this->top_left_connection->existPendingData()) {
        std::vector<DataPackage*> pck_vector = this->top_left_connection->receive();
        for(DataPackage* pck : pck_vector) {
            // Handle based on dataflow phase and requirements
            if(this->forward_diagonal) {
                this->bottom_right_fifo->push(pck);
            }
            else {
                this->top_left_fifo->push(pck);
            }
            // Would need to add stats tracking for diagonal receives
        }
    }
}

DataPackage* MultiplierWS::perform_operation_2_operands(DataPackage* pck_left, DataPackage* pck_right) {
    // Create a new package with the multiplication result
    float left_data = pck_left->get_data();
    float right_data = pck_right->get_data();
    float result = left_data * right_data; // Simple multiplication for now
    
    // Create a new package with the result
    DataPackage* result_pck = new DataPackage(sizeof(data_t), result, PSUM, this->num, this->VN, MULTIPLIER);
    
    // Track stats
    this->mswitchStats.n_multiplications++;
    
    return result_pck;
}

void MultiplierWS::preloadOperation() {
    // Handle the preload phase of WSDataflow
    // This typically involves loading weights/activations into local storage
    // before the main computation begins
    
    // The specific implementation would depend on your WSDataflow algorithm
}

void MultiplierWS::computeOperation() {
    // Handle the compute phase of WSDataflow
    // This is the main computation phase after preloading
    
    // Check if we have data in both FIFOs to perform multiplication
    if(!this->left_fifo->isEmpty() && !this->top_fifo->isEmpty()) {
        DataPackage* pck_left = this->left_fifo->pop();
        DataPackage* pck_top = this->top_fifo->pop();
        
        // Perform multiplication
        DataPackage* result = perform_operation_2_operands(pck_left, pck_top);
        
        // Send result to accumulation buffer or handle as needed
        this->accbuffer_fifo->push(result);
        
        // Clean up input packages if needed
        // delete pck_left;
        // delete pck_top;
    }
    
    // Specific processing based on your WSDataflow algorithm
}

void MultiplierWS::cycle() {
    this->local_cycle++;
    this->mswitchStats.total_cycles++;
    
    // Process based on current phase
    if(this->preload_phase) {
        preloadOperation();
    }
    else if(this->compute_phase) {
        computeOperation();
    }
    
    // Standard send/receive operations
    receive();
    send();
}

void MultiplierWS::resetSignals() {
    this->forward_right = false;
    this->forward_bottom = false;
    this->forward_diagonal = false;
    this->preload_phase = false;
    this->compute_phase = false;
    this->VN = 0;
}

void MultiplierWS::configureBottomSignal(bool bottom_signal) {
    this->forward_bottom = bottom_signal;
}

void MultiplierWS::configureRightSignal(bool right_signal) {
    this->forward_right = right_signal;
}

void MultiplierWS::configureDiagonalSignal(bool diagonal_signal) {
    this->forward_diagonal = diagonal_signal;
}

void MultiplierWS::setPreloadPhase(bool preload) {
    this->preload_phase = preload;
    if(preload) {
        this->compute_phase = false; // Ensure phases are mutually exclusive
    }
}

void MultiplierWS::setComputePhase(bool compute) {
    this->compute_phase = compute;
    if(compute) {
        this->preload_phase = false; // Ensure phases are mutually exclusive
    }
}

void MultiplierWS::setVirtualNeuron(unsigned int VN) {
    this->VN = VN;
}

void MultiplierWS::printConfiguration(std::ofstream& out, unsigned int indent) {
    // Implementation for printing configuration
}

void MultiplierWS::printStats(std::ofstream& out, unsigned int indent) {
    // Implementation for printing stats
}

void MultiplierWS::printEnergy(std::ofstream& out, unsigned int indent) {
    // Implementation for printing energy
}