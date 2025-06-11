#include "./include/Grid.h"
#include "./include/Connection.h"
#include "./include/TreeReducer.h"
#include "./include/Utility.h"


#include <vector>
#include <map>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <random>
#include <ctime>
#include <cmath>
#include <cassert>
#include <set>
#include <fstream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <future>

namespace fs = std::filesystem;

// Random number generator
double random_double(double min_val, double max_val) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(min_val, max_val);
    return dis(gen);
}

// Generate a random matrix with non-zeros only on specified diagonals
std::vector<std::vector<double>> generate_random_matrix(int size, const std::vector<int>& offsets, double min_val = 0.0, double max_val = 10.0) {
    std::vector<std::vector<double>> matrix(size, std::vector<double>(size, 0.0));
    for (int offset : offsets) {
        if (offset >= 0) {
            for (int i = 0; i < size - offset; i++) {
                int j = i + offset;
                matrix[i][j] = random_double(min_val, max_val);
            }
        } else {
            for (int i = -offset; i < size; i++) {
                int j = i + offset;
                matrix[i][j] = random_double(min_val, max_val);
            }
        }
    }
    return matrix;
}

// Generate symmetric offset combinations within range
std::vector<std::vector<int>> generate_symmetric_offsets(int range_size) {
    std::vector<std::vector<int>> result;
    int max_offset = range_size - 1;
    std::vector<int> all_offsets;
    for (int i = -max_offset; i <= max_offset; i++)
        all_offsets.push_back(i);

    auto generate_combinations = [](const std::vector<int>& positive_nums, int pairs_needed) {
        std::vector<std::vector<int>> combinations;
        int n = positive_nums.size();
        std::vector<bool> v(n);
        std::fill(v.end() - pairs_needed, v.end(), true);
        do {
            std::vector<int> pos_comb;
            for (int i = 0; i < n; ++i) {
                if (v[i]) pos_comb.push_back(positive_nums[i]);
            }
            std::vector<int> comb = pos_comb;
            for (int p : pos_comb) comb.push_back(-p);
            comb.push_back(0);
            std::sort(comb.begin(), comb.end());
            combinations.push_back(comb);
        } while (std::next_permutation(v.begin(), v.end()));
        return combinations;
    };

    for (int size = 1; size <= all_offsets.size(); size += 2) {
        std::vector<int> positive_nums;
        for (int x : all_offsets)
            if (x > 0) positive_nums.push_back(x);
        int pairs_needed = (size - 1) / 2;
        if (pairs_needed == 0) result.push_back({0});
        else {
            auto combs = generate_combinations(positive_nums, pairs_needed);
            result.insert(result.end(), combs.begin(), combs.end());
        }
    }

    std::sort(result.begin(), result.end(), [](const std::vector<int>& a, const std::vector<int>& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });

    return result;
}

// Extract diagonals
std::unordered_map<int, std::vector<double>> extract_diagonals(const std::vector<std::vector<double>>& matrix, const std::vector<int>& offsets) {
    int size = matrix.size();
    std::unordered_map<int, std::vector<double>> diagonals;
    for (int offset : offsets) {
        std::vector<double> diag;
        if (offset >= 0) {
            for (int i = 0; i < size - offset; i++) {
                diag.push_back(matrix[i][i + offset]);
            }
        } else {
            for (int i = -offset; i < size; i++) {
                diag.push_back(matrix[i][i + offset]);
            }
        }
        diagonals[offset] = diag;
    }
    return diagonals;
}

// Compute result diagonals from two sets of diagonals
std::vector<int> computeResultDiagonals(const std::vector<int>& A_diags, const std::vector<int>& B_diags) {
    std::set<int> result_set;
    for (int da : A_diags) {
        for (int db : B_diags) {
            result_set.insert(da + db);
        }
    }
    // Convert set to sorted vector
    return std::vector<int>(result_set.begin(), result_set.end());
}

// Convert diagonals to dense
std::vector<std::vector<double>> diagonals_to_dense(int size, const std::map<int, std::vector<double>>& diagonals) {
    std::vector<std::vector<double>> dense(size, std::vector<double>(size, 0.0));
    for (auto& [offset, vals] : diagonals) {
        if (offset >= 0) {
            for (int i = 0; i < vals.size(); i++) {
                dense[i][i + offset] = vals[i];
            }
        } else {
            for (int i = 0; i < vals.size(); i++) {
                dense[i - offset][i] = vals[i];
            }
        }
    }
    return dense;
}

// Dense multiplication
std::vector<std::vector<double>> dense_matrix_multiply(const std::vector<std::vector<double>>& A, const std::vector<std::vector<double>>& B) {
    int size = A.size();
    std::vector<std::vector<double>> C(size, std::vector<double>(size, 0.0));
    for (int i = 0; i < size; i++)
        for (int j = 0; j < size; j++)
            for (int k = 0; k < size; k++)
                C[i][j] += A[i][k] * B[k][j];
    return C;
}

// Print matrix
void print_matrix(const std::vector<std::vector<double>>& matrix, std::ofstream& out) {
    for (const auto& row : matrix) {
        for (double val : row) out << std::setw(12) <<val ;
        out << std::endl;
    }
}

std::vector<std::vector<DataPackage>> buildDatapackage(std::unordered_map<int, std::vector<double>>& diagonals) {
    std::vector<std::vector<DataPackage>> result;
    for (const auto& [offset, vals] : diagonals) {
        std::vector<DataPackage> packages;
        for (int i = 0; i < vals.size(); ++i) {
            if(offset >= 0) {
                packages.emplace_back(vals[i], i, i + offset);
            } else {
                packages.emplace_back(vals[i], i - offset, i);
            }

        }
        result.emplace_back(packages);
    }
    std::sort(result.begin(), result.end(), [](const std::vector<DataPackage>& a, const std::vector<DataPackage>& b) {
        if (a.empty() || b.empty()) return a.size() < b.size();
        return (a[0].index2 - a[0].index1) < (b[0].index2 - b[0].index1); // Sort by the width of the first DataPackage
    });
    return result;
}

int countMatrixMismatches(const std::vector<std::vector<double>>& ref,
                           const std::vector<std::vector<double>>& sim,
                           std::ofstream& out, double tolerance = 1e-6) {
    int mismatches = 0;

    if (ref.size() != sim.size()) {
        std::cerr << "Matrix row size mismatch!\n";
        return -1;
    }

    for (size_t i = 0; i < ref.size(); ++i) {
        if (ref[i].size() != sim[i].size()) {
            std::cerr << "Matrix column size mismatch at row " << i << "!\n";
            return -1;
        }

    for (size_t j = 0; j < ref[i].size(); ++j) {
        if (std::abs(ref[i][j] - sim[i][j]) > tolerance) {
            out << "Mismatch at (" << i << ", " << j << "): "
                      << "Reference = " << ref[i][j] << ", Simulated = " << sim[i][j] << "\n";
            ++mismatches;
        }
    }
    }
    return mismatches;
    
}


std::map<int, std::vector<double>> addMissingZeros(
    const std::map<int, std::vector<std::tuple<double, int, int>>>& diagonals,
    int size)
{
    std::map<int, std::vector<double>> result;

    for (const auto& [offset, entries] : diagonals) {
        int diag_len = size - std::abs(offset);  // length of the diagonal
        std::vector<double> diag(diag_len, 0.0); // initialize with zeros

        for (const auto& [value, i, j] : entries) {
            int pos = (offset >= 0) ? i : j;     // determine position in the diagonal vector
            diag[pos] = value;
        }

        result[offset] = diag;
    }

    return result;
}

void run_test_case(int i, const std::vector<int>& A_offsets, const std::vector<int>& B_offsets,
                    int& local_unsuccessful, int& local_test_cases, std::mutex& mtx, std::ofstream& out) {
    

    // Generate random matrices
    int size = i; // Fixed size for simplicity
    auto A = generate_random_matrix(size, A_offsets);
    auto B = generate_random_matrix(size, B_offsets);

    // Print generated matrices
    out << "Matrix A:\n";
    print_matrix(A, out);
    out << "Matrix B:\n";
    print_matrix(B, out);

    // Extract diagonals
    auto A_diag = extract_diagonals(A, A_offsets);
    auto B_diag = extract_diagonals(B, B_offsets);
    out << "Extracted Diagonals from A:\n";
    for (const auto& [offset, vals] : A_diag) {
        out << "Offset " << offset << ": ";
        for (double v : vals) out << v << " ";
        out << "\n";
    }
    out << "Extracted Diagonals from B:\n";
    for (const auto& [offset, vals] : B_diag) {
        out << "Offset " << offset << ": ";
        for (double v : vals) out << v << " ";
        out << "\n";
    }

    
    int COL = A_offsets.size();
    int ROW = B_offsets.size();

    Grid grid(ROW, COL, out);

    // Setup input connections
    std::vector<Connection*> left_in(ROW), top_in(COL);
    for (int i = 0; i < ROW; ++i) {
        left_in[i] = new Connection();
    }
    for (int i = 0; i < COL; ++i) {
        top_in[i] = new Connection();
    }
    grid.setInputConnections(top_in, left_in);

    // Setup output connections
    std::vector<Connection*> bottom_out(COL);
    for (int i = 0; i < COL; ++i)
        bottom_out[i] = new Connection();
    grid.setOutputConnections(bottom_out);

    // Setup TreeReducer for collecting psum + transfer
    TreeReducer reducer(bottom_out);
    
    //Convert diagonals to datapackages
    std::vector<std::vector<DataPackage>> A_diag_packages = buildDatapackage(A_diag);
    std::vector<std::vector<DataPackage>> B_diag_packages = buildDatapackage(B_diag);
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

    reducer.printResults();
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
    auto C_ref = dense_matrix_multiply(A, B);
    
    std::vector<int> C_offsets = computeResultDiagonals(A_offsets, B_offsets);

    // Extract diagonals from reference output
    auto C_ref_diag = extract_diagonals(C_ref, C_offsets);
    std::sort(C_offsets.begin(), C_offsets.end());
    out << "Reference Output Diagonals:\n";
    for (const auto& [offset, vals] : C_ref_diag) {
        out << "Offset " << offset << ": ";
        for (double v : vals) out << v << " ";
        out << "\n";
    }

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
        
        std::ofstream out("outputs/output_size_" + std::to_string(i) + ".txt");

        out << "Running test iteration " << i + 1 << std::endl;

        std::vector<std::vector<int>> offset_set = generate_symmetric_offsets(i);
        
        int local_test_cases = 0;
        int local_unsuccessful = 0;

        std::mutex mtx;
        std::vector<std::future<void>> futures;

        for(int j = 0; j < offset_set.size(); ++j) {
            for(int k = 0; k < offset_set.size(); ++k) {
                std::vector<int> A_offsets = offset_set[j];
                std::vector<int> B_offsets = offset_set[k];

                out << "A offsets: ";
                for (int a : A_offsets) out << a << " ";
                out << "\nB offsets: ";
                for (int b : B_offsets) out << b << " ";
                out << "\n";

                futures.emplace_back(std::async(std::launch::async, [&offset_set, j, k, i, &local_unsuccessful, &local_test_cases, &mtx, &out]() {
                run_test_case(i, offset_set[j], offset_set[k], local_unsuccessful, local_test_cases, mtx, out);
                }));
            }
        }

        for (auto& f : futures) f.get();

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

        //std::cout.rdbuf(cout_buf); // Restore original cout buffer

        // Create output directory if needed
        // fs::create_directory("outputs");

        // // Save to file
        // std::ofstream out("outputs/output_size_" + std::to_string(i) + ".txt");
        // out << oss.str();
        // out.close();
    }
    std::cout << "Total test cases: " << total_cases << "\n";
    std::cout << "Success: " << total_cases - total_unsuccessful << "\n";
    std::cout << "Successful Rate: " << (100.0 * (total_cases - total_unsuccessful) / total_cases) << "%" << "(" << total_cases - total_unsuccessful << "/" << total_cases << ")\n";

    return 0;
}
