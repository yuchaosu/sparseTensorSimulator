#include "./include/Grid.h"
#include "./include/Connection.h"
#include "./include/TreeReducer.h"
#include "./include/Utility.h"

#include <iostream>
#include <vector>
#include <fstream>
#include <mutex>
#include <future>
#include <sstream>
#include <algorithm>

// Random number generator


void run_test_case(int i, const std::vector<int>& A_offsets, 
                    int& local_unsuccessful, int& local_test_cases, std::mutex& mtx, std::ofstream& out) {
    

    // Generate random matrices
    int size = i; // Fixed size for simplicity
    std::vector<std::vector<double>> A = generate_random_matrix(size, A_offsets);
    std::vector<std::vector<double>> V = generate_random_vector(size);



    // Print generated matrices
    out << "Matrix A:\n";
    print_matrix(A, out);
    out << "Vector:\n";
    print_vector(V, out);

    // Extract diagonals
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> A_diag = extract_diagonals(A, A_offsets);
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> V_rebuild = rebuild_vector(V);


    out << "Extracted Diagonals from A:\n";
    for (const auto& [offset, vals] : A_diag) {
        out << "Offset " << offset << ": ";
        for (const auto& [val, i, j] : vals) out << val << " ";
        out << "\n";
    }

    
    int COL = A_offsets.size();
    int ROW = 1;

    Grid grid(ROW, COL, out);

    // Setup input connections
    std::vector<Connection*> left_in(ROW), top_in(COL);
    for (int i = 0; i < ROW; ++i) {
        left_in[i] = new Connection(out);
    }
    for (int i = 0; i < COL; ++i) {
        top_in[i] = new Connection(out);
    }
    grid.setInputConnections(top_in, left_in);

    // Setup output connections
    std::vector<Connection*> bottom_out(COL);
    for (int i = 0; i < COL; ++i)
        bottom_out[i] = new Connection(out);
    grid.setOutputConnections(bottom_out);

    // Setup TreeReducer for collecting psum + transfer
    TreeReducer reducer(bottom_out);
    
    //Convert diagonals to datapackages
    std::vector<std::vector<DataPackage>> A_diag_packages = buildDatapackage(A_diag);
    std::vector<std::vector<DataPackage>> B_diag_packages = buildDatapackage(V_rebuild);
    std::reverse(B_diag_packages.begin(), B_diag_packages.end()); // Reverse B packages to match the expected order
    out << "A Diagonal Packages:\n";
    for (const auto& diag : A_diag_packages) {
        for (const auto& dp : diag) {
            out << dp << " ";
        }
        out << "\n";
    }
    out << "B Diagonal Packages:\n";
    for (const auto& diag : B_diag_packages) {
        for (const auto& dp : diag) {
            out << dp << " ";
        }
        out << "\n";
    }
    int cycle = 0;
    // Run simulation
    
    bool injection_done = false;
    while(true) {
        out << "===== Cycle " << cycle << " =====\n";

        // Inject column data (A input)
        for (int col = 0; col < COL; ++col) {
            if (col < A_diag_packages.size()) {
                const auto& vec = A_diag_packages[col];
                int index = cycle - col;
                if (index >= 0 && index < vec.size()) {
                    if (!top_in[col]->pendingSrc()) {
                        top_in[col]->receiveSrc(vec[index]);
                    }
                }
            }
        }

        // Inject row data (B input)
        for (int row = 0; row < ROW; ++row) {
            if (row < B_diag_packages.size()) {
                const auto& vec = B_diag_packages[row];
                int index = cycle - row;
                if (index >= 0 && index < vec.size()) {
                    if (!left_in[row]->pendingSrc()) {
                        left_in[row]->receiveSrc(vec[index]);
                    }
                }
            }
        }
        
        injection_done = true;
        for (int i = 0; i < ROW; ++i) {
            if (left_in[i]->pendingSrc()) injection_done = false;
        }
        for (int i = 0; i < COL; ++i) {
            if (top_in[i]->pendingSrc()) injection_done = false;
        }

        grid.cycle();
        reducer.cycle();

        if(injection_done && grid.isIdle()) {
            out << "All data injected and processed. Breaking out of cycle loop.\n";
            break;
        }
        ++cycle;
    }

    //reducer.printResults();
    std::map<int, std::vector<double>> results = addMissingZeros(reducer.getResults(), size);
    out << "Simulated Final Results:\n";
    for (const auto& [key, vec] : results) {
        out << "Diagonal " << key << ": ";
        for (double val : vec) {
            out << val << " ";
        }
        out << "\n";
    }

    std::vector<std::vector<double>> simulated_results = diagonals_to_dense(size, results);


    // Perform dense multiplication
    std::vector<std::vector<double>> C_ref = matrix_vector_multiply(A, V);


    // Extract diagonals from reference output
    out << "Reference Output (Dense Matrix):\n";
    print_vector(C_ref, out);
   

    out << "Total Cycles: " << cycle << "\n";

    int mismatches = countMatrixMismatches(C_ref, simulated_results, out);
    std::lock_guard<std::mutex> lock(mtx);
 
    if (mismatches > 0) {
        out << "Mismatch found! Number of mismatches: " << mismatches << "\n";
        ++local_unsuccessful;
    }
    ++local_test_cases;

    out << "========================================\n";
    //delete
    for (auto* conn : left_in) delete conn;
    for (auto* conn : top_in) delete conn;
    for (auto* conn : bottom_out) delete conn;
}

int main() {
    int total_unsuccessful = 0;
    int total_cases = 0;
    std::cout << "Starting tests for symmetric offsets...\n";
    for(int i = 3; i < 9; ++i) {
        
        std::ofstream out("outputs/matrixVector_output_size_" + std::to_string(i) + ".txt");

        out << "Running test iteration " << i + 1 << std::endl;

        std::vector<std::vector<int>> offset_set = generate_symmetric_offsets(i);
        
        int local_test_cases = 0;
        int local_unsuccessful = 0;

        std::mutex mtx;
        std::vector<std::future<void>> futures;

        for(int j = 0; j < offset_set.size(); ++j) {

            std::vector<int> A_offsets = offset_set[j];


            out << "A offsets: ";
            for (int a : A_offsets) out << a << " ";

            run_test_case(i, A_offsets, local_unsuccessful, local_test_cases, mtx, out);

            // futures.emplace_back(std::async(std::launch::async, [&offset_set, j, i, &local_unsuccessful, &local_test_cases, &mtx, &out]() {
            // run_test_case(i, offset_set[j], local_unsuccessful, local_test_cases, mtx, out);
            // }));
        }

        //for (auto& f : futures) f.get();

        out << "Total test cases: " << local_test_cases << "\n";
        out << "Success: " << local_test_cases - local_unsuccessful << "\n";
        out << "Successful Rate: " << (100.0 * (local_test_cases - local_unsuccessful) / local_test_cases) << "%" << "(" << local_test_cases - local_unsuccessful << "/" << local_test_cases << ")\n";
        if (local_unsuccessful > 0) {
            out << "Total mismatches: " << local_unsuccessful << "\n";
        } else {
            out << "All tests passed successfully!\n";
        }
        total_unsuccessful += local_unsuccessful;
        total_cases += local_test_cases;
    }
    std::cout << "Total test cases: " << total_cases << "\n";
    std::cout << "Success: " << total_cases - total_unsuccessful << "\n";
    std::cout << "Successful Rate: " << (100.0 * (total_cases - total_unsuccessful) / total_cases) << "%" << "(" << total_cases - total_unsuccessful << "/" << total_cases << ")\n";

    return 0;
}
