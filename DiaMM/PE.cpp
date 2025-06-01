#include <iostream>
#include <vector>
#include <unordered_map>
#include <queue>
#include <tuple>
#include <algorithm>
#include <random>
#include <set>

using namespace std;

// Directions
enum Direction { N, S, W, E, NW, NE, SW, SE };

// Random number generator
int random_int(int min_val, int max_val) {
    static random_device rd;
    static mt19937 gen(rd());
    uniform_int_distribution<> dis(min_val, max_val);
    return dis(gen);
}

// Generate a random matrix with non-zeros only on specified diagonals
vector<vector<int>> generate_random_matrix(int size, const vector<int>& offsets, int min_val = 0, int max_val = 10) {
    vector<vector<int>> matrix(size, vector<int>(size, 0));
    for (int offset : offsets) {
        if (offset >= 0) {
            for (int i = 0; i < size - offset; i++) {
                int j = i + offset;
                matrix[i][j] = random_int(min_val, max_val);
            }
        } else {
            for (int i = -offset; i < size; i++) {
                int j = i + offset;
                matrix[i][j] = random_int(min_val, max_val);
            }
        }
    }
    return matrix;
}

// Generate symmetric offset combinations within range
vector<vector<int>> generate_symmetric_offsets(int range_size) {
    vector<vector<int>> result;
    int max_offset = range_size - 1;
    vector<int> all_offsets;
    for (int i = -max_offset; i <= max_offset; i++)
        all_offsets.push_back(i);

    // Helper for generating combinations
    auto generate_combinations = [](const vector<int>& positive_nums, int pairs_needed) {
        vector<vector<int>> combinations;
        int n = positive_nums.size();
        vector<bool> v(n);
        fill(v.end() - pairs_needed, v.end(), true);
        do {
            vector<int> pos_comb;
            for (int i = 0; i < n; ++i) {
                if (v[i]) pos_comb.push_back(positive_nums[i]);
            }
            vector<int> comb = pos_comb;
            for (int p : pos_comb) comb.push_back(-p);
            comb.push_back(0);
            sort(comb.begin(), comb.end());
            combinations.push_back(comb);
        } while (next_permutation(v.begin(), v.end()));
        return combinations;
    };

    for (int size = 1; size <= all_offsets.size(); size += 2) {
        vector<int> positive_nums;
        for (int x : all_offsets)
            if (x > 0) positive_nums.push_back(x);
        int pairs_needed = (size - 1) / 2;
        if (pairs_needed == 0) result.push_back({0});
        else {
            auto combs = generate_combinations(positive_nums, pairs_needed);
            result.insert(result.end(), combs.begin(), combs.end());
        }
    }

    // Sort by length and lex
    sort(result.begin(), result.end(), [](const vector<int>& a, const vector<int>& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });

    return result;
}

// Display matrix
// Helper to extract diagonals from a dense matrix
unordered_map<int, vector<int>> extract_diagonals(const vector<vector<int>>& matrix, const vector<int>& offsets) {
    int size = matrix.size();
    unordered_map<int, vector<int>> diagonals;
    for (int offset : offsets) {
        vector<int> diag;
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

// Helper to reconstruct a dense matrix from diagonal values
vector<vector<int>> diagonals_to_dense(int size, const unordered_map<int, vector<int>>& diagonals) {
    vector<vector<int>> dense(size, vector<int>(size, 0));
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

// Helper to multiply two dense matrices
vector<vector<int>> dense_matrix_multiply(const vector<vector<int>>& A, const vector<vector<int>>& B) {
    int size = A.size();
    vector<vector<int>> C(size, vector<int>(size, 0));
    for (int i = 0; i < size; i++)
        for (int j = 0; j < size; j++)
            for (int k = 0; k < size; k++)
                C[i][j] += A[i][k] * B[k][j];
    return C;
}

// Display matrix
void print_matrix(const vector<vector<int>>& matrix) {
    for (const auto& row : matrix) {
        for (int val : row) cout << val << "\t";
        cout << endl;
    }
}

// Buffer to hold diagonal values and manage injection
class Buffer {
public:
    unordered_map<int, vector<int>> A_diags, B_diags;
    unordered_map<int, int> g_times, h_times;
    unordered_map<int, int> next_A_idx, next_B_idx;

    void load_A_diag(int offset, const vector<int>& vals, int g_time) {
        A_diags[offset] = vals;
        g_times[offset] = g_time;
        next_A_idx[offset] = 0;
    }

    void load_B_diag(int offset, const vector<int>& vals, int h_time) {
        B_diags[offset] = vals;
        h_times[offset] = h_time;
        next_B_idx[offset] = 0;
    }

    // Get data for this cycle
    vector<tuple<int, int>> inject_A(int cycle, const unordered_map<int, int>& row_map) {
        vector<tuple<int, int>> injections;
        for (auto& [offset, vals] : A_diags) {
            if (cycle >= g_times[offset] && next_A_idx[offset] < vals.size()) {
                int idx = cycle - g_times[offset];
                if (idx == next_A_idx[offset]) {
                    injections.emplace_back(row_map.at(offset), vals[idx]);
                    next_A_idx[offset]++;
                }
            }
        }
        return injections;
    }

    vector<tuple<int, int>> inject_B(int cycle, const unordered_map<int, int>& col_map) {
        vector<tuple<int, int>> injections;
        for (auto& [offset, vals] : B_diags) {
            if (cycle >= h_times[offset] && next_B_idx[offset] < vals.size()) {
                int idx = cycle - h_times[offset];
                if (idx == next_B_idx[offset]) {
                    injections.emplace_back(col_map.at(offset), vals[idx]);
                    next_B_idx[offset]++;
                }
            }
        }
        return injections;
    }
};


// PE (Processing Element)
class PE {
public:
    int row, col;
    int a_val = 0, b_val = 0, psum = 0;
    int next_a = 0, next_b = 0, next_p = 0; // next-tick buffers

    PE(int r, int c) : row(r), col(c) {}

    void compute() {
        if (a_val && b_val) psum += a_val * b_val;
    }

    void forward(PE* east, PE* south, PE* southeast) {
        if (a_val && east) east->next_a = a_val;
        if (b_val && south) south->next_b = b_val;
        if (psum && southeast) southeast->next_p += psum;
    }

    void prepare_next_cycle() {
        a_val = next_a; next_a = 0;
        b_val = next_b; next_b = 0;
        psum = next_p; next_p = 0;
    }

    bool output_partial_sum(unordered_map<int, vector<int>>& result_diags, int row_offset, int col_offset) {
        if (psum != 0) {
            int diag_offset = row_offset + col_offset;
            result_diags[diag_offset].push_back(psum);
            return true;
        }
        return false;
    }
};

class Grid {
public:
    int rows, cols;
    vector<vector<PE*>> pes;
    Buffer buffer;
    vector<int> row_offsets, col_offsets;
    unordered_map<int, vector<int>> result_diagonals;

    Grid(vector<int> A_offsets, vector<int> B_offsets) {
        rows = A_offsets.size();
        cols = B_offsets.size();
        sort(A_offsets.begin(), A_offsets.end());
        sort(B_offsets.begin(), B_offsets.end());
        row_offsets = A_offsets;
        col_offsets = B_offsets;

        pes.resize(rows, vector<PE*>(cols));
        for (int i = 0; i < rows; i++)
            for (int j = 0; j < cols; j++)
                pes[i][j] = new PE(i, j);
    }

    void run(int max_cycles) {
        auto row_map = offset_to_index(row_offsets);
        auto col_map = offset_to_index(col_offsets);

        // Connection: add during run to handle SE
        for (int i = 0; i < rows; i++)
            for (int j = 0; j < cols; j++) {
                PE* pe = pes[i][j];
                PE* east = (j < cols - 1) ? pes[i][j+1] : nullptr;
                PE* south = (i < rows - 1) ? pes[i+1][j] : nullptr;
                PE* southeast = (i < rows - 1 && j > 0) ? pes[i+1][j-1] : nullptr;
                pe->next_a = pe->next_b = pe->next_p = 0; // clear next vals
            }

        for (int cycle = 0; cycle <= max_cycles; cycle++) {
            cout << "=== Cycle " << cycle << " ===\n";

            // Inject A
            auto a_inj = buffer.inject_A(cycle, row_map);
            for (auto [r, val] : a_inj) {
                pes[r][0]->a_val = val;
                cout << "Inject A diag into PE(" << r << ",0): " << val << endl;
            }

            // Inject B
            auto b_inj = buffer.inject_B(cycle, col_map);
            for (auto [c, val] : b_inj) {
                pes[0][c]->b_val = val;
                cout << "Inject B diag into PE(0," << c << "): " << val << endl;
            }

            // Compute and forward for all PEs
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    auto pe = pes[r][c];
                    PE* east = (c < cols - 1) ? pes[r][c+1] : nullptr;
                    PE* south = (r < rows - 1) ? pes[r+1][c] : nullptr;
                    PE* southeast = (r < rows - 1 && c > 0) ? pes[r+1][c-1] : nullptr;

                    pe->compute();
                    if (pe->psum != 0) {
                        cout << "PE(" << r << "," << c << ") computes: " << pe->a_val << " * " << pe->b_val << " = " << pe->psum << endl;
                    }
                    pe->forward(east, south, southeast);
                    if (pe->next_a != 0 || pe->next_b != 0 || pe->next_p != 0) {
                        cout << "PE(" << r << "," << c << ") forwards: " << pe->next_a << ", " << pe->next_b << ", " << pe->next_p << endl;
                    }
                }

            // Collect outputs
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    auto pe = pes[r][c];
                    if (!((r < rows - 1) && (c > 0)))  {// no southeast
                        pe->output_partial_sum(result_diagonals, row_offsets[r], col_offsets[c]);
                        cout << "PE(" << r << "," << c << ") outputs: " << pe->psum << endl;
                    }
                }

            // Prepare next cycle
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++)
                    pes[r][c]->prepare_next_cycle();
        }
    }

    vector<vector<int>> result_dense(int size) {
        return diagonals_to_dense(size, result_diagonals);
    }

    void print_result_diagonals() {
        for (auto& [offset, vals] : result_diagonals) {
            cout << "Diagonal " << offset << ": ";
            for (int val : vals) cout << val << " ";
            cout << endl;
        }
    }

private:
    unordered_map<int, int> offset_to_index(const vector<int>& offsets) {
        unordered_map<int, int> map;
        for (int i = 0; i < offsets.size(); i++)
            map[offsets[i]] = i;
        return map;
    }
};






int main() {
    // Generate symmetric offset patterns (example: range 3)
    auto offset_patterns = generate_symmetric_offsets(3);
    auto selected_offsets = offset_patterns[1];

    cout << "\nUsing Offsets: ";
    for (int o : selected_offsets) cout << o << " ";
    cout << endl;

    // Generate two random dense matrices A and B
    int size = 5;
    auto A = generate_random_matrix(size, selected_offsets, 1, 10);
    auto B = generate_random_matrix(size, selected_offsets, 1, 10);

    cout << "\nMatrix A:\n"; print_matrix(A);
    cout << "\nMatrix B:\n"; print_matrix(B);

    // Extract diagonals from A and B
    auto A_diags = extract_diagonals(A, selected_offsets);
    auto B_diags = extract_diagonals(B, selected_offsets);

    // Show reconstructed matrices from diagonals (sanity check)
    auto A_reconstructed = diagonals_to_dense(size, A_diags);
    auto B_reconstructed = diagonals_to_dense(size, B_diags);
    cout << "\nReconstructed Matrix A from Diagonals:\n"; print_matrix(A_reconstructed);
    cout << "\nReconstructed Matrix B from Diagonals:\n"; print_matrix(B_reconstructed);

    //print the diagonals of A and B
    cout << "\nDiagonals of A:\n";
    for (auto& [offset, vals] : A_diags) {
        cout << "Offset " << offset << ": ";
        for (int val : vals) cout << val << " ";
        cout << endl;
    }
    cout << "\nDiagonals of B:\n";
    for (auto& [offset, vals] : B_diags) {
        cout << "Offset " << offset << ": ";
        for (int val : vals) cout << val << " ";
        cout << endl;
    }
    
    // Setup Grid and Buffer
    Grid grid(selected_offsets, selected_offsets);
    int g_time = 0, h_time = 0;
    for (auto& [offset, vals] : A_diags) grid.buffer.load_A_diag(offset, vals, g_time);
    for (auto& [offset, vals] : B_diags) grid.buffer.load_B_diag(offset, vals, h_time);

    // Run simulation
    int max_cycles = 2 * size; // Enough cycles for data injection
    cout << "\n--- Running Systolic Array Simulation ---\n";
    grid.run(max_cycles);
    grid.print_result_diagonals();

    // Convert result diagonals to dense matrix
    // auto C_result = grid.result_dense(size);
    // cout << "\nResult Matrix C (from diagonal outputs):\n";
    // print_matrix(C_result);

    // Reference result from dense multiplication
    auto C_reference = dense_matrix_multiply(A, B);
    cout << "\nReference Matrix C (Dense Computation):\n";
    print_matrix(C_reference);

    return 0;
}