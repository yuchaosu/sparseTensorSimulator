#include "../include/Utility.h"
#include <random>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <set>


double random_double(double min_val, double max_val) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(min_val, max_val);
    return dis(gen);
}

std::vector<std::vector<double>> generate_random_matrix(int size, const std::vector<int>& offsets, double min_val, double max_val) {
    std::vector<std::vector<double>> matrix(size, std::vector<double>(size, 0.0));
    for (int offset : offsets) {
        if (offset >= 0) {
            for (int i = 0; i < size - offset; i++) {
                matrix[i][i + offset] = random_double(min_val, max_val);
            }
        } else {
            for (int i = -offset; i < size; i++) {
                matrix[i][i + offset] = random_double(min_val, max_val);
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
std::unordered_map<int, std::vector<std::tuple<double, int, int>>> 
extract_diagonals(const std::vector<std::vector<double>>& matrix, const std::vector<int>& offsets) {
    int size = matrix.size();
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> diagonals;

    for (int offset : offsets) {
        std::vector<std::tuple<double, int, int>> diag;
        if (offset >= 0) {
            for (int i = 0; i < size - offset; ++i) {
                int row = i;
                int col = i + offset;
                diag.emplace_back(matrix[row][col], row, col);
            }
        } else {
            for (int i = -offset; i < size; ++i) {
                int row = i;
                int col = i + offset;
                diag.emplace_back(matrix[row][col], row, col);
            }
        }
        diagonals[offset] = std::move(diag);
    }

    return diagonals;
}


// Compute result diagonals from two sets of diagonals
std::vector<int> computeResultDiagonals(const std::vector<int>& A_diags, const std::vector<int>& B_diags, int size) {
    std::set<int> result_set;
    for (int da : A_diags) {
        for (int db : B_diags) {
            if (std::abs(da + db) >= size) {
                // Skip if the resulting diagonal is out of bounds
                continue;
            }
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

std::vector<std::vector<DataPackage>> buildDatapackage(std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals) {
    std::vector<std::vector<DataPackage>> result;
    for (const auto& [offset, vals] : diagonals) {
        std::vector<DataPackage> packages;
        for (const auto& [val, i, j] : vals) {
            packages.emplace_back(val, i, j);
        }
        result.emplace_back(packages);
    }
    std::sort(result.begin(), result.end(), [](const std::vector<DataPackage>& a, const std::vector<DataPackage>& b) {
        if (a.empty() || b.empty()) return a.size() < b.size();
        return (a[0].index2 - a[0].index1) < (b[0].index2 - b[0].index1);
    });
    return result;
}


int countMatrixMismatches(const std::vector<std::vector<double>>& ref,
                           const std::vector<std::vector<double>>& sim,
                           std::ofstream& out, double tolerance) {
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

void show_progress_bar(int current, int total, int bar_width) {
    float progress = (float)current / total;
    int pos = (int)(bar_width * progress);

    std::cout << "[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << " %\r";
    std::cout.flush();
}

std::vector<std::vector<std::unordered_map<int, std::vector<double>>>>
split_diagonals_by_group(const std::unordered_map<int, std::vector<double>>& diagonals,
                         int num_groups,
                         const std::string& mode,  // "row" or "col"
                         int diagonal_num) {
    if (diagonals.find(0) == diagonals.end()) {
        throw std::invalid_argument("Main diagonal (offset 0) must be present to infer matrix size.");
    }

    int matrix_size = diagonals.at(0).size();
    if (num_groups > matrix_size) {
        throw std::invalid_argument("Number of groups cannot exceed matrix size.");
    }
    if (diagonal_num > diagonals.size()) {
        throw std::invalid_argument("diagonal_num cannot exceed number of input diagonals.");
    }

    // Extract and sort all diagonal offsets
    std::vector<int> all_offsets;
    for (const auto& [offset, _] : diagonals)
        all_offsets.push_back(offset);
    std::sort(all_offsets.begin(), all_offsets.end());

    // Partition offsets into diagonal_num groups
    std::vector<std::vector<int>> diagonal_offset_groups(diagonal_num);
    for (int i = 0; i < all_offsets.size(); ++i) {
        int d_group = i * diagonal_num / all_offsets.size();
        diagonal_offset_groups[d_group].push_back(all_offsets[i]);
    }

    // Final result: [num_groups][diagonal_num]
    std::vector<std::vector<std::unordered_map<int, std::vector<double>>>> result(
        num_groups, std::vector<std::unordered_map<int, std::vector<double>>>(diagonal_num));

    // Fill data
    for (int d_idx = 0; d_idx < diagonal_num; ++d_idx) {
        for (int offset : diagonal_offset_groups[d_idx]) {
            const auto& values = diagonals.at(offset);

            for (int i = 0; i < values.size(); ++i) {
                int row, col;
                if (offset >= 0) {
                    row = i;
                    col = i + offset;
                } else {
                    row = i - offset;
                    col = i;
                }

                int rc_group = -1;
                if (mode == "row") {
                    rc_group = row * num_groups / matrix_size;
                } else if (mode == "col") {
                    rc_group = col * num_groups / matrix_size;
                } else {
                    throw std::invalid_argument("Mode must be 'row' or 'col'.");
                }

                rc_group = std::min(rc_group, num_groups - 1);

                result[rc_group][d_idx][offset].push_back(values[i]);
            }
        }
    }

    return result;
}

std::pair<
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>,
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>
>
split_double_diagonals_by_group(
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals_A,
    const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diagonals_B,
    int num_groups,
    int diagonal_group_A,
    int diagonal_group_B) 
{
    if (diagonals_A.find(0) == diagonals_A.end() || diagonals_B.find(0) == diagonals_B.end()) {
        throw std::invalid_argument("Main diagonal (offset 0) must exist in both A and B.");
    }

    int matrix_size_A = diagonals_A.at(0).size();
    int matrix_size_B = diagonals_B.at(0).size();

    if (matrix_size_A != matrix_size_B) {
        throw std::invalid_argument("Matrix sizes of A and B must match.");
    }

    if (num_groups > matrix_size_A) {
        throw std::invalid_argument("Number of groups cannot exceed matrix size.");
    }

    if (diagonal_group_A > diagonals_A.size() || diagonal_group_B > diagonals_B.size()) {
        throw std::invalid_argument("Diagonal group count cannot exceed number of diagonals.");
    }

    // Helper updated for vector<tuple<double, int, int>>
    auto partition_offsets = [](const std::unordered_map<int, std::vector<std::tuple<double, int, int>>>& diags, int d_groups) {
        std::vector<std::vector<int>> result(d_groups);
        std::vector<int> all_offsets;
        for (const auto& [off, _] : diags) all_offsets.push_back(off);
        std::sort(all_offsets.begin(), all_offsets.end());
        for (int i = 0; i < all_offsets.size(); ++i) {
            int d_id = i * d_groups / all_offsets.size();
            result[d_id].push_back(all_offsets[i]);
        }
        return result;
    };

    auto offset_groups_A = partition_offsets(diagonals_A, diagonal_group_A);
    auto offset_groups_B = partition_offsets(diagonals_B, diagonal_group_B);

    int N = matrix_size_A;

    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>> grouped_A;
    std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>> grouped_B;

    // Fill A (col-wise)
    for (int d_idx = 0; d_idx < diagonal_group_A; ++d_idx) {
        for (int offset : offset_groups_A[d_idx]) {
            const auto& values = diagonals_A.at(offset);
            for (const auto& [val, row, col] : values) {
                int col_group = std::min(col * num_groups / N, num_groups - 1);
                grouped_A[col_group][d_idx][offset].emplace_back(val, row, col);
            }
        }
    }

    // Fill B (row-wise)
    for (int d_idx = 0; d_idx < diagonal_group_B; ++d_idx) {
        for (int offset : offset_groups_B[d_idx]) {
            const auto& values = diagonals_B.at(offset);
            for (const auto& [val, row, col] : values) {
                int row_group = std::min(row * num_groups / N, num_groups - 1);
                grouped_B[row_group][d_idx][offset].emplace_back(val, row, col);
            }
        }
    }

    return {grouped_A, grouped_B};
}





// Helper to print grouped diagonals
void print_groups(
    const std::map<int, std::map<int, std::unordered_map<int, std::vector<std::tuple<double, int, int>>>>>& grouped,
    std::ofstream& out) 
{
    for (const auto& [rc_group, diag_groups] : grouped) {
        out << "=== Row/Col Group " << rc_group << " ===\n";
        for (const auto& [d_group, offset_map] : diag_groups) {
            out << "  Diagonal Group " << d_group << ":\n";
            for (const auto& [offset, tuples] : offset_map) {
                out << "    Offset " << offset << ":\n";
                for (const auto& [val, i, j] : tuples) {
                    out << "      (" << i << ", " << j << ") = " << val << "\n";
                }
            }
        }
    }
}



std::vector<std::vector<double>> generate_random_vector(int size, double min_val, double max_val) {
    std::vector<std::vector<double>> matrix(size, std::vector<double>(1));  // size x 1 matrix
    for (int i = 0; i < size; ++i) {
        matrix[i][0] = random_double(min_val, max_val);
    }
    return matrix;
}

void print_vector(const std::vector<std::vector<double>>& vector, std::ofstream& out) {
    for (const auto& row : vector) {
        for (double val : row) out << std::setw(12) << val;
        out << std::endl;
    }
}

std::unordered_map<int, std::vector<std::tuple<double, int, int>>> 
rebuild_vector(const std::vector<std::vector<double>>& matrix) {
    std::unordered_map<int, std::vector<std::tuple<double, int, int>>> vector;
    std::vector<std::tuple<double, int, int>> entries;

    for (int i = 0; i < matrix.size(); ++i) {
        entries.emplace_back(matrix[i][0], i, 0);  // value, row, col (always col 0)
    }

    vector[0] = std::move(entries);  // store under offset 0
    return vector;
}


std::vector<std::vector<double>> matrix_vector_multiply(const std::vector<std::vector<double>>& A, const std::vector<std::vector<double>>& B) {
    int size = A.size();
    std::vector<std::vector<double>> C(size, std::vector<double>(1, 0.0));  // Result is also a column vector

    for (int i = 0; i < size; ++i)
        for (int k = 0; k < size; ++k)
            C[i][0] += A[i][k] * B[k][0];

    return C;
}
