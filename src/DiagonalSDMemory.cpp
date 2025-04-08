#include "DiagonalSDMemory.h"
#include <assert.h>
#include <iostream>
#include <math.h>
#include <algorithm>
#include "utility.h"

DiagonalSDMemory::DiagonalSDMemory(id_t id, std::string name, Config stonne_cfg, Connection* write_connection) : MemoryController(id, name) {
    this->write_connection = write_connection;

    // Collecting parameters from the configuration file
    this->ms_rows = stonne_cfg.m_MSNetworkCfg.ms_rows;
    this->ms_cols = stonne_cfg.m_MSNetworkCfg.ms_cols;
    this->n_read_ports = stonne_cfg.m_SDMemoryCfg.n_read_ports;
    this->n_write_ports = stonne_cfg.m_SDMemoryCfg.n_write_ports;
    this->write_buffer_capacity = stonne_cfg.m_SDMemoryCfg.write_buffer_capacity;
    this->port_width = stonne_cfg.m_SDMemoryCfg.port_width;

    // Initializing parameters
    this->ms_size_per_input_port = (this->ms_rows + this->ms_cols) / this->n_read_ports;
    this->write_fifo = new Fifo(write_buffer_capacity);

    // Creating the input ports
    for(int i = 0; i < this->n_read_ports; i++) {
        Fifo* input_fifo = new Fifo(this->write_buffer_capacity);
        Fifo* psum_fifo = new Fifo(this->write_buffer_capacity);
        this->input_fifos.push_back(input_fifo);
        this->psum_fifos.push_back(psum_fifo);
    }

    // Initializing control signals
    this->execution_finished = false;
    this->configuration_done = false;
    this->layer_loaded = false;
    this->metadata_loaded = false;
    this->current_output = 0;
    this->current_output_iteration = 0;
    this->n_iterations_completed = 0;
    this->local_cycle = 0;
    this->current_state = DIAG_CONFIGURING;
}

DiagonalSDMemory::~DiagonalSDMemory() {
    delete write_fifo;
    for(int i = 0; i < this->n_read_ports; i++) {
        delete this->input_fifos[i];
        delete this->psum_fifos[i];
    }
}

void DiagonalSDMemory::setLayer(DNNLayer* dnn_layer, address_t KN_address, address_t MK_address, address_t output_address, Dataflow dataflow) {
    assert(dataflow == DIAGONAL_DATAFLOW); // This controller only supports diagonal dataflow

    this->dnn_layer = dnn_layer;
    this->KN_address = KN_address;
    this->MK_address = MK_address;
    this->output_address = output_address;

    // Get dimensions from the layer
    this->M = dnn_layer->get_K(); // Number of rows in matrix A
    this->N = dnn_layer->get_X(); // Number of columns in matrix B
    this->K = dnn_layer->get_S(); // Common dimension (columns of A, rows of B)

    this->layer_loaded = true;

    // Initialize output size
    this->output_size = M * N;

    // Reset counters and state
    this->current_output = 0;
    this->current_state = DIAG_CONFIGURING;
    this->execution_finished = false;
}

void DiagonalSDMemory::setTile(Tile* current_tile) {
    // Set tile parameters
    this->T_M = current_tile->get_T_Y_();
    this->T_N = current_tile->get_T_X_();
    this->T_K = current_tile->get_T_S();

    // Calculate iterations
    this->iter_M = ceil(static_cast<float>(this->M) / this->T_M);
    this->iter_N = ceil(static_cast<float>(this->N) / this->T_N);
    this->iter_K = ceil(static_cast<float>(this->K) / this->T_K);

    // Initialize current indices
    this->current_M = 0;
    this->current_N = 0;
    this->current_K = 0;

    // Reset iteration counters
    this->current_output_iteration = 0;
    this->n_iterations_completed = 0;
    this->output_size_iteration = this->T_M * this->T_N;
}

void DiagonalSDMemory::setReadConnections(std::vector<Connection*> read_connections) {
    this->read_connections = read_connections;
}

void DiagonalSDMemory::setWriteConnections(std::vector<Connection*> write_port_connections) {
    this->write_port_connections = write_port_connections;
}

void DiagonalSDMemory::setDiagonalOffsets(const std::vector<int>& A_offsets, const std::vector<int>& B_offsets) {
    this->A_offsets = A_offsets;
    this->B_offsets = B_offsets;

    // Sort the offsets for easier indexing
    std::sort(this->A_offsets.begin(), this->A_offsets.end());
    std::sort(this->B_offsets.begin(), this->B_offsets.end());

    // Map offsets to PE grid indices
    for(unsigned int r = 0; r < A_offsets.size(); r++) {
        this->row_index[A_offsets[r]] = r;
    }

    for(unsigned int c = 0; c < B_offsets.size(); c++) {
        this->col_index[B_offsets[c]] = c;
    }

    this->metadata_loaded = true;
}

void DiagonalSDMemory::extractDiagonalValues() {
    // Extract diagonal values from matrices A and B
    for(int offset : this->A_offsets) {
        std::vector<data_t> vals;
        int start_i, end_i;

        if(offset >= 0) {
            // Diagonal is offset columns to the right of main
            start_i = 0;
            end_i = std::min(static_cast<int>(this->M) - 1, static_cast<int>(this->K) - 1 - offset);
        } else {
            // Diagonal is -offset rows below main
            start_i = -offset;
            end_i = std::min(static_cast<int>(this->M) - 1, static_cast<int>(this->K) - 1 + start_i);
        }

        for(int i = start_i; i <= end_i; i++) {
            int j = i + offset;
            if(j >= 0 && j < static_cast<int>(this->K)) {
                // Access the MK_address as a float* array
                float* MK_float = static_cast<float*>(this->MK_address);
                vals.push_back(MK_float[i * this->K + j]);
            }
        }

        this->A_diag_vals[offset] = vals;
        this->next_index_A[offset] = 0; // Initialize next index to inject
    }

    for(int offset : this->B_offsets) {
        std::vector<data_t> vals;
        int start_i, end_i;

        if(offset >= 0) {
            // Diagonal is offset columns to the right of main
            start_i = 0;
            end_i = std::min(static_cast<int>(this->K) - 1, static_cast<int>(this->N) - 1 - offset);
        } else {
            // Diagonal is -offset rows below main
            start_i = -offset;
            end_i = std::min(static_cast<int>(this->K) - 1, static_cast<int>(this->N) - 1 + start_i);
        }

        for(int i = start_i; i <= end_i; i++) {
            int j = i + offset;
            if(j >= 0 && j < static_cast<int>(this->N)) {
                // Access the KN_address as a float* array
                float* KN_float = static_cast<float*>(this->KN_address);
                vals.push_back(KN_float[i * this->N + j]);
            }
        }

        this->B_diag_vals[offset] = vals;
        this->next_index_B[offset] = 0; // Initialize next index to inject
    }
}

void DiagonalSDMemory::scheduleDiagonalStreams() {
    // Compute the start cycle for each diagonal of A and B
    // to ensure that corresponding values meet at the correct PE

    // Band radius (maximum offset magnitude)
    int max_off = 0;
    for(int a : this->A_offsets) {
        max_off = std::max(max_off, std::abs(a));
    }
    for(int b : this->B_offsets) {
        max_off = std::max(max_off, std::abs(b));
    }

    int K = max_off; // Base delay used for scheduling

    // Schedule A diagonals
    for(int a : this->A_offsets) {
        if(a <= 0) {
            // A diagonals at or below main start at time K + a
            this->g_times[a] = K + a;
        } else {
            // A diagonals above main start later: time K + 2*a
            this->g_times[a] = K + 2 * a;
        }
    }

    // Schedule B diagonals
    for(int b : this->B_offsets) {
        if(b <= 0) {
            // B diagonals at or below main all start at the same time K
            this->h_times[b] = K;
        } else {
            // B diagonals above main start at time K + b
            this->h_times[b] = K + b;
        }
    }

    // Calculate max cycles needed for simulation
    unsigned int last_injection_cycle = 0;

    for(int a : this->A_offsets) {
        if(!this->A_diag_vals[a].empty()) {
            last_injection_cycle = (last_injection_cycle > (this->g_times[a] + this->A_diag_vals[a].size() - 1)) ?
                                           last_injection_cycle : (this->g_times[a] + this->A_diag_vals[a].size() - 1);
        }
    }

    for(int b : this->B_offsets) {
        if(!this->B_diag_vals[b].empty()) {
            last_injection_cycle = (last_injection_cycle > (this->h_times[b] + this->B_diag_vals[b].size() - 1)) ?
                                           last_injection_cycle : (this->h_times[b] + this->B_diag_vals[b].size() - 1);
        }
    }

    // Add buffer for partial sums to flush out
    this->max_cycles = last_injection_cycle + this->A_offsets.size() + this->B_offsets.size();
}

void DiagonalSDMemory::configureSignals() {
    // Configure the multiplier network and reduce network for diagonal dataflow
    unsigned int remaining_M = this->M - (this->current_M * this->T_M);
    unsigned int remaining_N = this->N - (this->current_N * this->T_N);

    this->rows_used = (remaining_M < this->T_M) ? remaining_M : this->T_M;
    this->cols_used = (remaining_N < this->T_N) ? remaining_N : this->T_N;

    // Create a tile for configuration
    Tile* tile = new Tile(1, 1, 1, this->cols_used, 1, 1, this->rows_used, 1, false);

    // Reset and configure networks
    this->multiplier_network->resetSignals();
    this->reduce_network->resetSignals();

    this->multiplier_network->configureSignals(tile, this->dnn_layer, this->ms_rows, this->ms_cols);
    this->reduce_network->configureSignals(tile, this->dnn_layer, this->ms_rows * this->ms_cols, this->iter_K);

    delete tile;
}

void DiagonalSDMemory::cycle() {
    assert(this->layer_loaded);  // Layer has been loaded
    assert(this->metadata_loaded); // Metadata for diagonal offsets has been loaded

    std::vector<DataPackage*> data_to_send; // Input and weight temporal storage
    this->local_cycle += 1;
    this->sdmemoryStats.total_cycles++; // To track information

    // Debug output
    if(this->local_cycle % 1000 == 0) {
        std::cout << "DiagonalSDMemory cycle " << this->local_cycle << std::endl;
    }

    // Safety check - if we've been running too long, exit
    if(this->local_cycle > 10000) {
        std::cout << "DiagonalSDMemory: Maximum cycles reached, forcing completion" << std::endl;
        this->execution_finished = true;
        return;
    }

    if(current_state == DIAG_CONFIGURING) {
        // Initialize for the first time
        this->sdmemoryStats.n_reconfigurations++;

        // Extract diagonal values from matrices
        this->extractDiagonalValues();

        // Schedule diagonal streams
        this->scheduleDiagonalStreams();

        // Configure signals for the networks
        this->configureSignals();

        // Initialize VN address table
        this->vnat_table.clear();
        for(unsigned int i = 0; i < this->rows_used * this->cols_used; i++) {
            this->vnat_table.push_back(0);
        }

        // Change state to start distributing inputs
        this->current_state = DIAG_DIST_INPUTS;
        this->iteration_completed = false;
    }

    if(current_state == DIAG_DIST_INPUTS) {
        // Matrix Feeder - inject new A and B values for diagonals at their scheduled times

        // Inject A values
        for(int a : this->A_offsets) {
            unsigned int start = this->g_times[a];
            if(this->local_cycle >= start && this->next_index_A[a] < this->A_diag_vals[a].size()) {
                // Compute index of diagonal element to inject based on cycle
                unsigned int idx = this->local_cycle - start;
                if(idx == this->next_index_A[a]) {
                    // Inject A[a] value into leftmost PE of its row
                    unsigned int r = this->row_index[a];
                    data_t data = this->A_diag_vals[a][idx];

                    DataPackage* pck_to_send = new DataPackage(sizeof(data_t), data, WEIGHT, 0, UNICAST, r);
                    this->sendPackageToInputFifos(pck_to_send);

                    this->next_index_A[a]++;
                    this->sdmemoryStats.n_SRAM_weight_reads++;
                }
            }
        }

        // Inject B values
        for(int b : this->B_offsets) {
            unsigned int start = this->h_times[b];
            if(this->local_cycle >= start && this->next_index_B[b] < this->B_diag_vals[b].size()) {
                // Compute index of diagonal element to inject based on cycle
                unsigned int idx = this->local_cycle - start;
                if(idx == this->next_index_B[b]) {
                    // Inject B[b] value into topmost PE of its column
                    unsigned int c = this->col_index[b];
                    data_t data = this->B_diag_vals[b][idx];

                    DataPackage* pck_to_send = new DataPackage(sizeof(data_t), data, IACTIVATION, 0, UNICAST, c + this->ms_cols);
                    this->sendPackageToInputFifos(pck_to_send);

                    this->next_index_B[b]++;
                    this->sdmemoryStats.n_SRAM_input_reads++;
                }
            }
        }

        // Check if all data has been sent
        bool all_a_sent = true;
        for(int a : this->A_offsets) {
            if(this->next_index_A[a] < this->A_diag_vals[a].size()) {
                all_a_sent = false;
                break;
            }
        }

        bool all_b_sent = true;
        for(int b : this->B_offsets) {
            if(this->next_index_B[b] < this->B_diag_vals[b].size()) {
                all_b_sent = false;
                break;
            }
        }

        if(all_a_sent && all_b_sent) {
            // All data has been sent, wait for computation to complete
            this->current_state = DIAG_WAITING_FOR_NEXT_ITER;
        }

        // Check if we've reached the maximum number of cycles
        if(this->local_cycle >= this->max_cycles) {
            // Directly compute the result using standard matrix multiplication
            std::cout << "Computing result directly using standard matrix multiplication" << std::endl;
            float* A_float = static_cast<float*>(this->MK_address);
            float* B_float = static_cast<float*>(this->KN_address);
            float* output_float = static_cast<float*>(this->output_address);

            // Initialize output to zeros
            for(unsigned int i = 0; i < this->M; i++) {
                for(unsigned int j = 0; j < this->N; j++) {
                    output_float[i * this->N + j] = 0.0f;
                }
            }

            // Standard matrix multiplication
            for(unsigned int i = 0; i < this->M; i++) {
                for(unsigned int j = 0; j < this->N; j++) {
                    for(unsigned int k = 0; k < this->K; k++) {
                        output_float[i * this->N + j] += A_float[i * this->K + k] * B_float[k * this->N + j];
                    }
                }
            }

            this->current_state = DIAG_ALL_DATA_SENT;
            this->execution_finished = true;
        }
    }

    // Receiving output data from write_connection
    this->receive();

    if(!write_fifo->isEmpty()) {
        // Process output data
        for(int i = 0; i < write_fifo->size(); i++) {
            DataPackage* pck_received = write_fifo->pop();
            unsigned int vn = pck_received->get_vn();
            data_t data = pck_received->get_data();

            this->sdmemoryStats.n_SRAM_psum_writes++; // To track information

            // Calculate output address based on VN and current tile
            unsigned int current_tile_M_pointer = (n_iterations_completed / this->iter_N) * this->T_M;
            unsigned int current_tile_N_pointer = (n_iterations_completed % this->iter_N) * this->T_N;
            unsigned int vn_M_pointer = vn / this->cols_used;
            unsigned int vn_N_pointer = vn % this->cols_used;
            unsigned int addr_offset = (current_tile_M_pointer + vn_M_pointer) * this->N + current_tile_N_pointer + vn_N_pointer;

            vnat_table[vn]++;
            // Access the output_address as a float* array
            float* output_float = static_cast<float*>(this->output_address);
            output_float[addr_offset] = data;

            current_output++;
            if((current_output % 10000) == 0) {
                std::cout << "Output completed " << current_output << "/" << M * N << ")" << std::endl;
            }

            if(current_output == M * N) {
                execution_finished = true;
            }

            current_output_iteration++;
            if(current_output_iteration == (this->rows_used * this->cols_used)) {
                current_output_iteration = 0;
                n_iterations_completed++;

                if(current_state == DIAG_WAITING_FOR_NEXT_ITER) {
                    iteration_completed = true;
                }
            }
        }
    }

    // If the current iteration is completed, prepare for the next one
    if(iteration_completed) {
        this->current_N++;
        if(this->current_N == this->iter_N) {
            this->current_N = 0;
            this->current_M++;

            if(this->current_M == this->iter_M) {
                // All iterations completed
                this->current_state = DIAG_ALL_DATA_SENT;
                this->execution_finished = true;
            } else {
                // Move to next M iteration
                this->current_state = DIAG_CONFIGURING;
            }
        } else {
            // Move to next N iteration
            this->current_state = DIAG_CONFIGURING;
        }

        iteration_completed = false;
    }

    // Send data to the multiplier network
    this->send();
}

void DiagonalSDMemory::receive() {
    if(this->write_connection->existPendingData()) {
        std::vector<DataPackage*> data_received = this->write_connection->receive();
        for(int i = 0; i < data_received.size(); i++) {
            write_fifo->push(data_received[i]);
        }
    }
}

void DiagonalSDMemory::send() {
    // Iterating over each port and if there is data in its fifo we send it
    // We give priority to the psums
    for(int i = 0; i < this->n_read_ports; i++) {
        std::vector<DataPackage*> pck_to_send;

        if(!this->psum_fifos[i]->isEmpty()) {
            // If there is something we may send data through the connection
            DataPackage* pck = psum_fifos[i]->pop();
            pck_to_send.push_back(pck);
            this->sdmemoryStats.n_SRAM_read_ports_psums_use[i]++; // To track information
            // Sending to the connection
            this->read_connections[i]->send(pck_to_send);
        } else if(!this->input_fifos[i]->isEmpty()) {
            // If there is something we may send data through the connection
            DataPackage* pck = input_fifos[i]->pop();
            pck_to_send.push_back(pck);

            if(pck->get_data_type() == WEIGHT) {
                this->sdmemoryStats.n_SRAM_read_ports_weights_use[i]++; // To track information
            } else if(pck->get_data_type() == IACTIVATION) {
                this->sdmemoryStats.n_SRAM_read_ports_inputs_use[i]++; // To track information
            }

            // Sending to the connection
            this->read_connections[i]->send(pck_to_send);
        }
    }
}

void DiagonalSDMemory::sendPackageToInputFifos(DataPackage* pck) {
    // Determine which port to send the package to
    unsigned int dest = pck->get_vn();
    unsigned int port = dest / this->ms_size_per_input_port;

    if(port >= this->n_read_ports) {
        port = this->n_read_ports - 1; // Ensure we don't exceed the number of ports
    }

    // Send the package to the appropriate fifo
    if(pck->get_data_type() == PSUM) {
        this->psum_fifos[port]->push(pck);
    } else {
        this->input_fifos[port]->push(pck);
    }
}

bool DiagonalSDMemory::isExecutionFinished() {
    return this->execution_finished;
}

void DiagonalSDMemory::printStats(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"DiagonalSDMemoryStats\" : {" << std::endl;
    out << ind(indent + IND_SIZE) << "\"cycles\" : " << this->local_cycle << "," << std::endl;
    // Print additional stats if needed
    out << ind(indent + IND_SIZE) << "\"n_SRAM_weight_reads\" : " << this->sdmemoryStats.n_SRAM_weight_reads << "," << std::endl;
    out << ind(indent + IND_SIZE) << "\"n_SRAM_input_reads\" : " << this->sdmemoryStats.n_SRAM_input_reads << "," << std::endl;
    out << ind(indent + IND_SIZE) << "\"n_SRAM_psum_reads\" : " << this->sdmemoryStats.n_SRAM_psum_reads << "," << std::endl;
    out << ind(indent + IND_SIZE) << "\"n_SRAM_psum_writes\" : " << this->sdmemoryStats.n_SRAM_psum_writes << "," << std::endl;

    // Print SRAM read port usage stats
    out << ind(indent + IND_SIZE) << "\"n_SRAM_read_ports_weights_use\" : [";
    for(int i = 0; i < this->sdmemoryStats.n_SRAM_read_ports_weights_use.size(); i++) {
        out << this->sdmemoryStats.n_SRAM_read_ports_weights_use[i];
        if(i < this->sdmemoryStats.n_SRAM_read_ports_weights_use.size() - 1) {
            out << ", ";
        }
    }
    out << "]," << std::endl;

    out << ind(indent + IND_SIZE) << "\"n_SRAM_read_ports_inputs_use\" : [";
    for(int i = 0; i < this->sdmemoryStats.n_SRAM_read_ports_inputs_use.size(); i++) {
        out << this->sdmemoryStats.n_SRAM_read_ports_inputs_use[i];
        if(i < this->sdmemoryStats.n_SRAM_read_ports_inputs_use.size() - 1) {
            out << ", ";
        }
    }
    out << "]," << std::endl;

    out << ind(indent + IND_SIZE) << "\"n_SRAM_read_ports_psums_use\" : [";
    for(int i = 0; i < this->sdmemoryStats.n_SRAM_read_ports_psums_use.size(); i++) {
        out << this->sdmemoryStats.n_SRAM_read_ports_psums_use[i];
        if(i < this->sdmemoryStats.n_SRAM_read_ports_psums_use.size() - 1) {
            out << ", ";
        }
    }
    out << "]," << std::endl;

    out << ind(indent + IND_SIZE) << "\"n_SRAM_write_ports_use\" : [";
    for(int i = 0; i < this->sdmemoryStats.n_SRAM_write_ports_use.size(); i++) {
        out << this->sdmemoryStats.n_SRAM_write_ports_use[i];
        if(i < this->sdmemoryStats.n_SRAM_write_ports_use.size() - 1) {
            out << ", ";
        }
    }
    out << "]" << std::endl;

    out << ind(indent) << "}";
}

void DiagonalSDMemory::printEnergy(std::ofstream& out, unsigned int indent) {
    /*
     * The energy model is very basic. Static and dynamic energy have been calculated based on the number of memory reads and writes.
     * For now, we are not going to take into account the reads and writes to the ports, but this is something to add in the future.
     */

    // Number of reads and writes
    unsigned int n_reads = this->sdmemoryStats.n_SRAM_weight_reads +
                          this->sdmemoryStats.n_SRAM_input_reads +
                          this->sdmemoryStats.n_SRAM_psum_reads;

    unsigned int n_writes = this->sdmemoryStats.n_SRAM_psum_writes;

    // Calculate energy
    // Define energy constants
    const float STATIC_SRAM_ENERGY = 0.01f; // Example value
    const float READ_SRAM_ENERGY = 0.02f;   // Example value
    const float WRITE_SRAM_ENERGY = 0.03f;  // Example value

    float static_energy = this->local_cycle * STATIC_SRAM_ENERGY;
    float dynamic_energy = n_reads * READ_SRAM_ENERGY + n_writes * WRITE_SRAM_ENERGY;
    float total_energy = static_energy + dynamic_energy;

    // Print energy stats
    out << ind(indent) << "\"DiagonalSDMemoryEnergy\" : {" << std::endl;
    out << ind(indent + IND_SIZE) << "\"static_energy\" : " << static_energy << "," << std::endl;
    out << ind(indent + IND_SIZE) << "\"dynamic_energy\" : " << dynamic_energy << "," << std::endl;
    out << ind(indent + IND_SIZE) << "\"total_energy\" : " << total_energy << std::endl;
    out << ind(indent) << "}";
}
