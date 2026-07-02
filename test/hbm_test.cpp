#include "../include/Utility.h"
#include "../include/hbm_diagonal_writer.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <tuple>

int main(){
    std::string filename = "H_array_graph-1D-grid-nonpbc-qubitnodes_Lx-10_h-0_sparse.txt";
    std::string folder = "/mnt/beegfs/ysu34/hamlib/heis/";
    int size = 1024; // Example size
    std::string output_name = filename;
    size_t dot_pos = output_name.find_last_of(".");
    if (dot_pos != std::string::npos) {
        output_name = output_name.substr(0, dot_pos);
    }
    // std::ofstream out("/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/output_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_DBlocked.log");
    // std::ofstream Energyout("/mnt/beegfs/ysu34/" + std::to_string(qubit_size) + "/output_" + std::to_string(grid_row) + "x" + std::to_string(grid_col) + "_DBlocked.power");
 
    std::cout << "filename: " << folder + filename << "\n";
    std::string basePath = folder;

    // Load the first matrix
    std::string filenameA = basePath + filename;
    std::vector<int> A_offsets = extractDiagonalOffsets(filenameA);
    auto current_diag = createDiagonalMap(filenameA, A_offsets, size);

    AcceleratorHBM::HBMController hbm(8);
    hbm.init(4ULL * 1024 * 1024 * 1024);  // e.g. 4 GiB total HBM
    hbm.startup(0);   // optional if you simulate from t=0
    HBMDiagIO::writeDiagonalsAndReportBandwidth(hbm, current_diag, size);


}