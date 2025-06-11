//Created on 22/10/2019 by Francisco Munoz Martinez

#include "Config.h"
#include <iostream>
#include "types.h"
#include "utility.h"
#include "cpptoml.h"

Config::Config() {
    this->reset();
}

bool Config::loadFile(std::string config_file) {
    try {
        auto config = cpptoml::parse_file(config_file);

        //General parameters
        auto print_stats_enabled_conf = config->get_as<bool>("print_stats_enabled");  //print_stats_enabled
        if(print_stats_enabled_conf) {
            this->print_stats_enabled = *print_stats_enabled_conf;
        }

        //DSNetwork Configuration Parameters
        auto n_switches_traversed_by_cycle_conf = config->get_qualified_as<unsigned int>("DSNetwork.n_switches_traversed_by_cycle"); //n_switches_traversed_by_cycle
        if(n_switches_traversed_by_cycle_conf) {
            this->m_DSNetworkCfg.n_switches_traversed_by_cycle = *n_switches_traversed_by_cycle_conf;
        }

        //DSwitch Configuration Parameters
        auto dswitch_latency_conf = config->get_qualified_as<unsigned int>("DSwitchCfg.latency"); //Latency
        if(dswitch_latency_conf) {
            this->m_DSwitchCfg.latency = *dswitch_latency_conf;
        }

        auto dswitch_input_ports_conf = config->get_qualified_as<unsigned int>("DSwitchCfg.input_ports");  //input_ports
        if(dswitch_input_ports_conf) {
            this->m_DSwitchCfg.input_ports = *dswitch_input_ports_conf;
        }

        auto dswitch_output_ports_conf = config->get_qualified_as<unsigned int>("DSwitchCfg.output_ports");  //output_ports
        if(dswitch_output_ports_conf) {
            this->m_DSwitchCfg.output_ports = *dswitch_output_ports_conf;
        }

        auto dswitch_port_width_conf = config->get_qualified_as<unsigned int>("DSwitchCfg.port_width");  //port_width
        if(dswitch_port_width_conf) {
            this->m_DSwitchCfg.port_width = *dswitch_port_width_conf;
        }

        auto dswitch_forwarding_ports_conf = config->get_qualified_as<unsigned int>("DSwitchCfg.forwarding_ports");  //forwarding_ports
        if(dswitch_forwarding_ports_conf) {
            this->m_DSwitchCfg.forwarding_ports = *dswitch_forwarding_ports_conf;
        }

        auto dswitch_buffers_capacity_conf = config->get_qualified_as<unsigned int>("DSwitchCfg.buffers_capacity");  //buffers_capacity
        if(dswitch_buffers_capacity_conf) {
            this->m_DSwitchCfg.buffers_capacity = *dswitch_buffers_capacity_conf;
        }

        //MSNetwork Configuration Parameters
        auto ms_size_conf = config->get_qualified_as<unsigned int>("MSNetwork.ms_size");
        if(ms_size_conf) {
            this->m_MSNetworkCfg.ms_size=*ms_size_conf;
        }

        auto ms_rows_conf = config->get_qualified_as<unsigned int>("MSNetwork.ms_rows");
        if(ms_rows_conf) {
            this->m_MSNetworkCfg.ms_rows=*ms_rows_conf;
        }

        auto ms_cols_conf = config->get_qualified_as<unsigned int>("MSNetwork.ms_cols");
        if(ms_cols_conf) {
            this->m_MSNetworkCfg.ms_cols=*ms_cols_conf;
        }

        auto multiplier_network_type_conf = config->get_qualified_as<std::string>("MSNetwork.multiplier_network_type");  //Buffers_capacity
        if(multiplier_network_type_conf) {
            this->m_MSNetworkCfg.multiplier_network_type = get_type_multiplier_network_type(*multiplier_network_type_conf);
        }

        //MSwitch Configuration parameters
        auto mswitch_latency_conf = config->get_qualified_as<unsigned int>("MSwitchCfg.latency"); //latency
        if(mswitch_latency_conf) {
            this->m_MSwitchCfg.latency = *mswitch_latency_conf;
        }

        auto mswitch_input_ports_conf = config->get_qualified_as<unsigned int>("MSwitchCfg.input_ports"); //input_ports
        if(mswitch_input_ports_conf) {
            this->m_MSwitchCfg.input_ports = *mswitch_input_ports_conf;
        }

        auto mswitch_output_ports_conf = config->get_qualified_as<unsigned int>("MSwitchCfg.output_ports"); //output_ports
        if(mswitch_output_ports_conf) {
            this->m_MSwitchCfg.output_ports = *mswitch_output_ports_conf;
        }

        auto mswitch_forwarding_ports_conf = config->get_qualified_as<unsigned int>("MSwitchCfg.forwarding_ports"); //forwarding_ports
        if(mswitch_forwarding_ports_conf) {
            this->m_MSwitchCfg.forwarding_ports = *mswitch_forwarding_ports_conf;
        }

        auto mswitch_port_width_conf = config->get_qualified_as<unsigned int>("MSwitchCfg.port_width"); //port_width
        if(mswitch_port_width_conf) {
            this->m_MSwitchCfg.port_width = *mswitch_port_width_conf;
        }

        auto mswitch_buffers_capacity_conf = config->get_qualified_as<unsigned int>("MSwitchCfg.buffers_capacity"); //buffers_capacity
        if(mswitch_buffers_capacity_conf) {
            this->m_MSwitchCfg.buffers_capacity = *mswitch_buffers_capacity_conf;
        }

        //ASNetwork Configuration Parameters
        auto reduce_network_type_conf = config->get_qualified_as<std::string>("ASNetwork.reduce_network_type"); //reduce_network_type
        if(reduce_network_type_conf) {
            this->m_ASNetworkCfg.reduce_network_type = get_type_reduce_network_type(*reduce_network_type_conf);
        }

        auto accumulation_buffer_enabled_conf = config->get_qualified_as<unsigned int>("ASNetwork.accumulation_buffer_enabled"); //accumulation_buffer_enabled
        if(accumulation_buffer_enabled_conf) {
            this->m_ASNetworkCfg.accumulation_buffer_enabled = *accumulation_buffer_enabled_conf;
        }

        //ASwitch Configuration Parameters
        auto aswitch_latency_conf = config->get_qualified_as<unsigned int>("ASwitchCfg.latency"); //latency
        if(aswitch_latency_conf) {
            this->m_ASwitchCfg.latency = *aswitch_latency_conf;
        }

        auto aswitch_input_ports_conf = config->get_qualified_as<unsigned int>("ASwitchCfg.input_ports"); //input_ports
        if(aswitch_input_ports_conf) {
            this->m_ASwitchCfg.input_ports = *aswitch_input_ports_conf;
        }

        auto aswitch_output_ports_conf = config->get_qualified_as<unsigned int>("ASwitchCfg.output_ports"); //output_ports
        if(aswitch_output_ports_conf) {
            this->m_ASwitchCfg.output_ports = *aswitch_output_ports_conf;
        }

        auto aswitch_forwarding_ports_conf = config->get_qualified_as<unsigned int>("ASwitchCfg.forwarding_ports"); //forwarding_ports
        if(aswitch_forwarding_ports_conf) {
            this->m_ASwitchCfg.forwarding_ports = *aswitch_forwarding_ports_conf;
        }

        auto aswitch_port_width_conf = config->get_qualified_as<unsigned int>("ASwitchCfg.port_width"); //port_width
        if(aswitch_port_width_conf) {
            this->m_ASwitchCfg.port_width = *aswitch_port_width_conf;
        }

        auto aswitch_buffers_capacity_conf = config->get_qualified_as<unsigned int>("ASwitchCfg.buffers_capacity"); //buffers_capacity
        if(aswitch_buffers_capacity_conf) {
            this->m_ASwitchCfg.buffers_capacity = *aswitch_buffers_capacity_conf;
        }

        //LookUpTable Configuration Parameters
        auto lookuptable_latency_conf = config->get_qualified_as<unsigned int>("LookUpTable.latency"); //latency
        if(lookuptable_latency_conf) {
            this->m_LookUpTableCfg.latency = *lookuptable_latency_conf;
        }

        auto lookuptable_port_width_conf = config->get_qualified_as<unsigned int>("LookUpTable.port_width"); //port_width
        if(lookuptable_port_width_conf) {
            this->m_LookUpTableCfg.port_width = *lookuptable_port_width_conf;
        }

        //SDMemory Configuration Parameters
        auto mem_controller_type_conf = config->get_qualified_as<std::string>("SDMemory.mem_controller_type"); //mem_controller_type
        if(mem_controller_type_conf) {
            this->m_SDMemoryCfg.mem_controller_type = get_type_memory_controller_type(*mem_controller_type_conf);
        }

        auto write_buffer_capacity_conf = config->get_qualified_as<unsigned int>("SDMemory.write_buffer_capacity"); //write_buffer_capacity
        if(write_buffer_capacity_conf) {
            this->m_SDMemoryCfg.write_buffer_capacity = *write_buffer_capacity_conf;
        }

        auto n_read_ports_conf = config->get_qualified_as<unsigned int>("SDMemory.n_read_ports"); //n_read_ports
        if(n_read_ports_conf) {
            this->m_SDMemoryCfg.n_read_ports = *n_read_ports_conf;
        }

        auto n_write_ports_conf = config->get_qualified_as<unsigned int>("SDMemory.n_write_ports"); //n_write_ports
        if(n_write_ports_conf) {
            this->m_SDMemoryCfg.n_write_ports = *n_write_ports_conf;
        }

        auto sdmemory_port_width_conf = config->get_qualified_as<unsigned int>("SDMemory.port_width"); //port_width
        if(sdmemory_port_width_conf) {
            this->m_SDMemoryCfg.port_width = *sdmemory_port_width_conf;
        }

        return true;
    } catch(const std::exception& e) {
        std::cerr << "Error loading config file: " << e.what() << std::endl;
        return false;
    }
}

void Config::reset() {
    //General parameters
    print_stats_enabled=1;
    diagonal_dataflow_enabled=false;

    // ---------------------------------------------------------
    // DSNetwork Configuration Parameters
    // ---------------------------------------------------------
    this->m_DSNetworkCfg.n_switches_traversed_by_cycle=1;

    // ---------------------------------------------------------
    // DSwitch Configuration Parameters
    // ---------------------------------------------------------
    this->m_DSwitchCfg.latency=1;
    this->m_DSwitchCfg.input_ports=1;
    this->m_DSwitchCfg.output_ports=2;
    this->m_DSwitchCfg.port_width=8;
    this->m_DSwitchCfg.forwarding_ports=1;
    this->m_DSwitchCfg.buffers_capacity=1;

    // ---------------------------------------------------------
    // MSNetwork Configuration Parameters
    // ---------------------------------------------------------
    this->m_MSNetworkCfg.ms_size=64;
    this->m_MSNetworkCfg.ms_rows=8;
    this->m_MSNetworkCfg.ms_cols=8;
    this->m_MSNetworkCfg.multiplier_network_type=LINEAR;

    // ---------------------------------------------------------
    // MSwitch Configuration Parameters
    // ---------------------------------------------------------
    this->m_MSwitchCfg.latency=1;
    this->m_MSwitchCfg.input_ports=1;
    this->m_MSwitchCfg.output_ports=1;
    this->m_MSwitchCfg.forwarding_ports=1;
    this->m_MSwitchCfg.port_width=8;
    this->m_MSwitchCfg.buffers_capacity=1;

    // ---------------------------------------------------------
    // ASNetwork Configuration Parameters
    // ---------------------------------------------------------
    this->m_ASNetworkCfg.reduce_network_type=ASNETWORK;
    this->m_ASNetworkCfg.accumulation_buffer_enabled=1;

    // ---------------------------------------------------------
    // ASwitch Configuration Parameters
    // ---------------------------------------------------------
    this->m_ASwitchCfg.latency=1;
    this->m_ASwitchCfg.input_ports=2;
    this->m_ASwitchCfg.output_ports=1;
    this->m_ASwitchCfg.forwarding_ports=1;
    this->m_ASwitchCfg.port_width=8;
    this->m_ASwitchCfg.buffers_capacity=1;

    // ---------------------------------------------------------
    // LookUpTable Configuration Parameters
    // ---------------------------------------------------------
    this->m_LookUpTableCfg.latency=1;
    this->m_LookUpTableCfg.port_width=8;

    // ---------------------------------------------------------
    // SDMemory Configuration Parameters
    // ---------------------------------------------------------
    this->m_SDMemoryCfg.mem_controller_type=MAERI_DENSE_WORKLOAD;
    this->m_SDMemoryCfg.write_buffer_capacity=64;
    this->m_SDMemoryCfg.n_read_ports=1;
    this->m_SDMemoryCfg.n_write_ports=1;
    this->m_SDMemoryCfg.port_width=8;
    this->m_SDMemoryCfg.n_multiplier_configurations=1;
    this->m_SDMemoryCfg.n_reduce_network_configurations=1;
}

bool Config::sparsitySupportEnabled() {
    return (this->m_SDMemoryCfg.mem_controller_type == SIGMA_SPARSE_GEMM);
}

bool Config::convOperationSupported() {
    return (this->m_SDMemoryCfg.mem_controller_type == MAERI_DENSE_WORKLOAD);
}

// -----------------------------------------------------------------------------------------------
// Config printing function
// -----------------------------------------------------------------------------------------------
void Config::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"Config\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"print_stats_enabled\" : " << this->print_stats_enabled << "," << std::endl;
    this->m_DSNetworkCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_DSwitchCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_MSNetworkCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_MSwitchCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_ASNetworkCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_ASwitchCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_LookUpTableCfg.printConfiguration(out, indent+IND_SIZE);
    out << "," << std::endl;
    this->m_SDMemoryCfg.printConfiguration(out, indent+IND_SIZE);
    out << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// DSNetworkConfig printing function
// -----------------------------------------------------------------------------------------------
void DSNetworkConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"DSNetwork\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"n_switches_traversed_by_cycle\" : " << this->n_switches_traversed_by_cycle << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// DSwitchConfig printing function
// -----------------------------------------------------------------------------------------------
void DSwitchConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"DSwitch\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"latency\" : " << this->latency << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"input_ports\" : " << this->input_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"output_ports\" : " << this->output_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"port_width\" : " << this->port_width << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// MSNetworkConfig printing function
// -----------------------------------------------------------------------------------------------
void MSNetworkConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"MSNetwork\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"ms_size\" : " << this->ms_size << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"ms_rows\" : " << this->ms_rows << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"ms_cols\" : " << this->ms_cols << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"multiplier_network_type\" : " << "\"" << get_string_multiplier_network_type(this->multiplier_network_type) << "\"" << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// MSwitchConfig printing function
// -----------------------------------------------------------------------------------------------
void MSwitchConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"MSwitch\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"latency\" : " << this->latency << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"input_ports\" : " << this->input_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"output_ports\" : " << this->output_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"forwarding_ports\" : " << this->forwarding_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"port_width\" : " << this->port_width << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"buffers_capacity\" : " << this->buffers_capacity << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// ASNetworkConfig printing function
// -----------------------------------------------------------------------------------------------
void ASNetworkConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"ReduceNetwork\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"reduce_network_type\" : " << "\"" << get_string_reduce_network_type(this->reduce_network_type) << "\"" << ","  << std::endl;
    out << ind(indent+IND_SIZE) << "\"accumulation_buffer_enabled\" : " << this->accumulation_buffer_enabled  << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// ASwitchConfig printing function
// -----------------------------------------------------------------------------------------------
void ASwitchConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"ASwitch\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"latency\" : " << this->latency << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"input_ports\" : " << this->input_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"output_ports\" : " << this->output_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"forwarding_ports\" : " << this->forwarding_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"port_width\" : " << this->port_width << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"buffers_capacity\" : " << this->buffers_capacity  << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// LookUpTaleConfig printing function
// -----------------------------------------------------------------------------------------------
void LookUpTableConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"LookUpTable\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"latency\" : " << this->latency << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"port_width\" : " << this->port_width  << std::endl;
    out << ind(indent) << "}";
}

// -----------------------------------------------------------------------------------------------
// SDMemoryConfig printing function
// -----------------------------------------------------------------------------------------------
void SDMemoryConfig::printConfiguration(std::ofstream& out, unsigned int indent) {
    out << ind(indent) << "\"SDMemory\" : {" << std::endl;
    out << ind(indent+IND_SIZE) << "\"mem_controller_type\" : " << "\"" << get_string_memory_controller_type(this->mem_controller_type) << "\""  << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"write_buffers_capacity\" : " << this->write_buffer_capacity << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"dn_bw\" : " << this->n_read_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"rn_bw\" : " << this->n_write_ports << "," << std::endl;
    out << ind(indent+IND_SIZE) << "\"port_width\" : " << this->port_width << std::endl;
    out << ind(indent) << "}";
}
