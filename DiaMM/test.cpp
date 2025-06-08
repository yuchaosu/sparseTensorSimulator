#include <iostream>
#include <vector>
#include <map>
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


using namespace std;

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

    sort(result.begin(), result.end(), [](const vector<int>& a, const vector<int>& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });

    return result;
}

// Extract diagonals
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

// Convert diagonals to dense
vector<vector<int>> diagonals_to_dense(int size, const map<int, vector<int>>& diagonals) {
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

// Dense multiplication
vector<vector<int>> dense_matrix_multiply(const vector<vector<int>>& A, const vector<vector<int>>& B) {
    int size = A.size();
    vector<vector<int>> C(size, vector<int>(size, 0));
    for (int i = 0; i < size; i++)
        for (int j = 0; j < size; j++)
            for (int k = 0; k < size; k++)
                C[i][j] += A[i][k] * B[k][j];
    return C;
}

// Print matrix
void print_matrix(const vector<vector<int>>& matrix) {
    for (const auto& row : matrix) {
        for (int val : row) cout << val << "\t";
        cout << endl;
    }
}


// Schedule diagonal streams for A and B
pair<map<int, int>, map<int, int>> schedule_diagonal_streams(const vector<int>& A_offsets, const vector<int>& B_offsets) {
    int max_off = 0;
    for (int a : A_offsets) max_off = max({max_off, abs(a)});
    for (int b : B_offsets) max_off = max({max_off, abs(b)});
    int K = max_off;

    map<int, int> g_times, h_times;
    for (int a : A_offsets)
        g_times[a] = (a <= 0) ? a + K : 2 * a + K;
    for (int b : B_offsets)
        h_times[b] = (b <= 0) ? K : b + K;
    cout << "K = " << K << endl;
    return {g_times, h_times};
}

// Matrix multiplication using the systolic array simulator
tuple<map<int, vector<int>>, string, int> simulate_systolic_array(
    const vector<vector<int>>& A,
    const vector<vector<int>>& B,
    const vector<int>& A_offsets,
    const vector<int>& B_offsets)
{
    int N = A.size();
    auto A_diag_vals = extract_diagonals(A, A_offsets);
    auto B_diag_vals = extract_diagonals(B, B_offsets);
    //print the A_diag_vals and B_diag_vals
    cout << "\nA_diag_vals:\n";
    for (auto& [offset, vals] : A_diag_vals) {
        cout << "Offset " << offset << ": ";
        for (int v : vals) cout << v << " ";
        cout << endl;
    }
    cout << "\nB_diag_vals:\n";
    for (auto& [offset, vals] : B_diag_vals) {
        cout << "Offset " << offset << ": ";
        for (int v : vals) cout << v << " ";
        cout << endl;
    }

    auto [g_times, h_times] = schedule_diagonal_streams(A_offsets, B_offsets);
    //print the g_times and h_times
    cout << "\ng_times:\n";
    for (auto& [a, t] : g_times) cout << "  Offset " << a << ": " << t << endl;
    cout << "\nh_times:\n";
    for (auto& [b, t] : h_times) cout << "  Offset " << b << ": " << t << endl;
    vector<int> A_sorted = A_offsets;
    vector<int> B_sorted = B_offsets;
    //print the A_sorted and B_sorted
    cout << "\nA_sorted: ";
    for (int a : A_sorted) cout << a << " ";
    cout << endl;
    cout << "\nB_sorted: ";
    for (int b : B_sorted) cout << b << " ";
    cout << endl;
    sort(A_sorted.begin(), A_sorted.end());
    sort(B_sorted.begin(), B_sorted.end());

    map<int, int> row_index, col_index;
    for (int i = 0; i < A_sorted.size(); ++i) row_index[A_sorted[i]] = i;
    for (int i = 0; i < B_sorted.size(); ++i) col_index[B_sorted[i]] = i;
    int R = A_sorted.size(), C = B_sorted.size();

    vector<vector<int>> A_in(R, vector<int>(C, 0)), B_in(R, vector<int>(C, 0)), P_in(R, vector<int>(C, 0));
    map<int, int> next_A, next_B;
    for (int a : A_offsets) next_A[a] = 0;
    for (int b : B_offsets) next_B[b] = 0;

    int last_injection = 0;
    for (int a : A_offsets)
        last_injection = max(last_injection, g_times[a] + static_cast<int>(A_diag_vals[a].size()) - 1);
    for (int b : B_offsets)
        last_injection = max(last_injection, h_times[b] + static_cast<int>(B_diag_vals[b].size()) - 1);
    int max_cycles = last_injection + R + C;

    ostringstream log;
    int total_active_cycles = 0;

    map<int, vector<int>> sim_diag_out;  // Collect simulator diagonal outputs

    
    cout << "g_times:\n";
    for (auto& [a, t] : g_times) cout << "  Offset " << a << ": " << t << endl;
    cout << "h_times:\n";
    for (auto& [b, t] : h_times) cout << "  Offset " << b << ": " << t << endl;
    for (auto& [a, vals] : A_diag_vals) {
        cout << "A[" << a << "] length: " << vals.size() << endl;
    }
    for (auto& [b, vals] : B_diag_vals) {
        cout << "B[" << b << "] length: " << vals.size() << endl;
    }

    //print the initial state of A_in, B_in, P_in
    for (int cycle = 0; cycle <= max_cycles; ++cycle) {
        vector<string> events;

        // Inject A
        for (int a : A_offsets) {
            int start = g_times[a];
            if (cycle >= start && next_A[a] < A_diag_vals[a].size()) {
                int idx = cycle - start;
                if (idx == next_A[a]) {
                    int r = row_index[a];
                    A_in[r][0] = A_diag_vals[a][idx];
                    next_A[a]++;
                    events.push_back("Inject A[" + to_string(a) + "][" + to_string(idx) + "] into PE(" + to_string(r) + ",0)");
                }
            }
        }
        // Inject B
        for (int b : B_offsets) {
            int start = h_times[b];
            if (cycle >= start && next_B[b] < B_diag_vals[b].size()) {
                int idx = cycle - start;
                if (idx == next_B[b]) {
                    int c = col_index[b];
                    B_in[0][c] = B_diag_vals[b][idx];
                    next_B[b]++;
                    events.push_back("Inject B[" + to_string(b) + "][" + to_string(idx) + "] into PE(0," + to_string(c) + ")");
                }
            }
        }

        vector<vector<int>> nxt_A(R, vector<int>(C, 0)), nxt_B(R, vector<int>(C, 0)), nxt_P(R, vector<int>(C, 0));

        // PE processing
        for (int r = 0; r < R; ++r) {
            for (int c = 0; c < C; ++c) {
                int a_val = A_in[r][c];
                int b_val = B_in[r][c];
                int psum = P_in[r][c];
                int prod = (a_val && b_val) ? a_val * b_val : 0;
                int psum_out = psum + prod;
                int diag = A_sorted[r] + B_sorted[c];
                events.push_back("PE[" + to_string(r) + "][" + to_string(c) + "] A=" + to_string(a_val) + " B=" + to_string(b_val) 
                     + " psum=" + to_string(psum) + " prod=" + to_string(prod) + " out=" + to_string(psum_out) 
                     + " diag=" + to_string(diag));

                if (r < R-1 && c > 0)
                    nxt_P[r+1][c-1] += psum_out;
                if ((r == R-1 || c == 0) && psum_out != 0) {
                    sim_diag_out[diag].push_back(psum_out);
                    events.push_back("Output val " + to_string(psum_out) + " for C_diag=" + to_string(diag));
                }

                if (c < C-1 && a_val) nxt_A[r][c+1] = a_val;
                if (r < R-1 && b_val) nxt_B[r+1][c] = b_val;
            }
        }

        if (!events.empty()) {
            log << "### Cycle " << cycle << "\n";
            for (const auto& e : events) log << "- " << e << "\n";
            total_active_cycles = cycle + 1;
        }

        A_in = nxt_A; B_in = nxt_B; P_in = nxt_P;
        bool injections_done = true;
        for (auto& [a, idx] : next_A) {
            if (idx < A_diag_vals[a].size()) {
                injections_done = false;
                break;
            }
        }
        for (auto& [b, idx] : next_B) {
            if (idx < B_diag_vals[b].size()) {
                injections_done = false;
                break;
            }
        }

        bool grid_empty = true;
        for (int r = 0; r < R; ++r) {
            for (int c = 0; c < C; ++c) {
                if (A_in[r][c] || B_in[r][c] || P_in[r][c]) {
                    grid_empty = false;
                    break;
                }
            }
            if (!grid_empty) break;
        }

        if (injections_done && grid_empty) {
            log << "\nSimulation finished at cycle " << cycle << ".\n";
            break;
        }

    }

    return {sim_diag_out, log.str(), total_active_cycles};
}

int main() {
    int size = 5;

    // Generate symmetric offset patterns (example: range 3)
    auto offset_patterns = generate_symmetric_offsets(3);
    auto selected_offsets = offset_patterns[1];  // Choose a pattern, e.g. the 2nd pattern

    cout << "\nUsing Offsets: ";
    for (int o : selected_offsets) cout << o << " ";
    cout << endl;

    auto A = generate_random_matrix(size, selected_offsets, 1, 10);
    auto B = generate_random_matrix(size, selected_offsets, 1, 10);
    //vector<vector<int>> A = {{1,0,3,0,0},{0,7,0,9,0},{11,0,13,0,15},{0,17,0,19,0},{0,0,23,0,25}};
    //vector<vector<int>> B = {{1,0,3,0,0},{0,7,0,9,0},{11,0,13,0,15},{0,17,0,19,0},{0,0,23,0,25}};

    cout << "\nMatrix A:\n"; print_matrix(A);
    cout << "\nMatrix B:\n"; print_matrix(B);

    //print the selected_offsets
    cout << "\nSelected Offsets: ";
    for (int o : selected_offsets) cout << o << " ";
    cout << endl;

    auto [sim_diag_out, log, cycles] = simulate_systolic_array(A, B, selected_offsets, selected_offsets);

    cout << "\nSystolic Array Simulation Log:\n" << log;
    cout << "\nTotal Active Cycles: " << cycles << "\n";

    cout << "\nSimulator Output Diagonals:\n";
    for (auto& [offset, vals] : sim_diag_out) {
        cout << "Offset " << offset << ": ";
        for (int v : vals) cout << v << " ";
        cout << "\n";
    }


    // Reference dense matrix multiplication
    auto C_ref = dense_matrix_multiply(A, B);
    cout << "\nReference Output Matrix:\n";
    print_matrix(C_ref);

    auto C_ref_diag = extract_diagonals(C_ref, {-4,-2,0,2,4});

    cout << "\nReference Output Diagonals:\n";
    for (auto& [offset, vals] : C_ref_diag) {
        cout << "Offset " << offset << ": ";
        for (int v : vals) cout << v << " ";
        cout << "\n";
    }

    // Verification
    cout << "\nVerification of Simulator Output:\n";
    bool passed = true;
    //verify if C_sim_diag is equal to C_ref_diag
    for (auto& [offset, vals] : sim_diag_out) {
        for (int i = 0; i < vals.size(); i++) {
            if (vals[i] != C_ref_diag[offset][i]) {
                passed = false;
                cout << "Mismatch at (" << offset << "," << i << "): Simulator=" << vals[i] 
                     << ", Reference=" << C_ref_diag[offset][i] << "\n";
            }
        }
    }

    if (passed)
        cout << "\nVerification PASSED: Simulator output matches reference.\n";
    else
        cout << "\nVerification FAILED: Simulator output differs from reference.\n";

    return 0;
}

// int main() {
//     ofstream fout("systolic_array_test_log.txt");
//     const int max_diag_limit = 7;

//     for (int size = 3; size <= 8; ++size) {
//         fout << "\n\n=== MATRIX SIZE: " << size << "x" << size << " ===\n";

//         int diag_limit = min(2 * size - 1, max_diag_limit);
//         auto offset_patterns = generate_symmetric_offsets(diag_limit / 2);

//         for (int i = 0; i < offset_patterns.size(); ++i) {
//             for (int j = 0; j < offset_patterns.size(); ++j) {
//                 auto A_offsets = offset_patterns[i];
//                 auto B_offsets = offset_patterns[j];

//                 fout << "\n-- A_offsets: ";
//                 for (int a : A_offsets) fout << a << " ";
//                 fout << "\n-- B_offsets: ";
//                 for (int b : B_offsets) fout << b << " ";
//                 fout << endl;

//                 auto A = generate_random_matrix(size, A_offsets, 1, 10);
//                 auto B = generate_random_matrix(size, B_offsets, 1, 10);

//                 auto [sim_diag_out, log, cycles] = simulate_systolic_array(A, B, A_offsets, B_offsets);
//                 auto C_ref = dense_matrix_multiply(A, B);

//                 // Expected offsets from combinations a + b
//                 set<int> res_offsets;
//                 for (int a : A_offsets)
//                     for (int b : B_offsets)
//                         res_offsets.insert(a + b);
//                 vector<int> res_offsets_vec(res_offsets.begin(), res_offsets.end());

//                 auto C_ref_diag = extract_diagonals(C_ref, res_offsets_vec);

//                 bool passed = true;
//                 for (int off : res_offsets_vec) {
//                     const auto& sim_vals = sim_diag_out[off];
//                     const auto& ref_vals = C_ref_diag[off];
//                     if (sim_vals != ref_vals) {
//                         passed = false;
//                         fout << "  MISMATCH in offset " << off << ":\n    Simulator: ";
//                         for (int v : sim_vals) fout << v << " ";
//                         fout << "\n    Reference: ";
//                         for (int v : ref_vals) fout << v << " ";
//                         fout << "\n";
//                     }
//                 }

//                 fout << "  => Result: " << (passed ? "✅ PASSED" : "❌ FAILED") << "\n";
//                 fout << "  Total Cycles: " << cycles << "\n";
//             }
//         }
//     }

//     fout.close();
//     cout << "\nAll tests completed. Results written to systolic_array_test_log.txt\n";
//     return 0;
// }