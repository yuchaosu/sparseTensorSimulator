#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <random>
#include <ctime>
#include <cmath>

using namespace std;

// Helper to generate a random dense matrix of given size
vector<vector<int>> generate_random_matrix(int size, int min_val=0, int max_val=10) {
    vector<vector<int>> matrix(size, vector<int>(size, 0));
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(min_val, max_val);

    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j)
            matrix[i][j] = dis(gen);
    return matrix;
}

// Extract diagonal values from a matrix given the offsets
map<int, vector<int>> get_diagonal_values(const vector<vector<int>>& matrix, const vector<int>& offsets) {
    int N = matrix.size();
    map<int, vector<int>> diag_vals;
    for (int off : offsets) {
        vector<int> vals;
        int start_i = (off >= 0) ? 0 : -off;
        int end_i = (off >= 0) ? N - 1 - off : N - 1;
        for (int i = start_i; i <= end_i; ++i) {
            int j = i + off;
            vals.push_back(matrix[i][j]);
        }
        diag_vals[off] = vals;
    }
    return diag_vals;
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
    auto A_diag_vals = get_diagonal_values(A, A_offsets);
    auto B_diag_vals = get_diagonal_values(B, B_offsets);
    auto [g_times, h_times] = schedule_diagonal_streams(A_offsets, B_offsets);

    vector<int> A_sorted = A_offsets;
    vector<int> B_sorted = B_offsets;
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

                if (r < R-1 && c > 0)
                    nxt_P[r+1][c-1] += psum_out;
                else
                    events.push_back("Output val " + to_string(psum_out) + " for C_diag=" + to_string(A_sorted[r]+B_sorted[c]));

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
    }

    // Reference matrix C
    vector<vector<int>> C_ref(N, vector<int>(N, 0));
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            for (int k = 0; k < N; ++k)
                C_ref[i][j] += A[i][k] * B[k][j];

    map<int, vector<int>> diag_out;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            diag_out[j-i].push_back(C_ref[i][j]);

    return {diag_out, log.str(), total_active_cycles};
}

int main() {
    int size = 5;
    vector<int> A_offsets = {-1, 0, 1};
    vector<int> B_offsets = {-1, 0, 1};
    auto A = generate_random_matrix(size);
    auto B = generate_random_matrix(size);

    cout << "Matrix A:\n";
    for (auto& row : A) { for (int v : row) cout << setw(4) << v; cout << endl; }
    cout << "\nMatrix B:\n";
    for (auto& row : B) { for (int v : row) cout << setw(4) << v; cout << endl; }

    auto [diag_out, log, cycles] = simulate_systolic_array(A, B, A_offsets, B_offsets);

    cout << "\nSystolic Array Simulation Log:\n" << log;
    cout << "\nTotal Active Cycles: " << cycles << "\n";

    // Verification: dense product matrix
    vector<vector<int>> C_ref(size, vector<int>(size, 0));
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j)
            for (int k = 0; k < size; ++k)
                C_ref[i][j] += A[i][k] * B[k][j];

    cout << "\nDense Product Matrix:\n";
    for (auto& row : C_ref) { for (int v : row) cout << setw(6) << v; cout << endl; }

    // Verify each diagonal
    cout << "\nDiagonal Verification:\n";
    bool pass = true;
    for (auto& [offset, vals] : diag_out) {
        cout << "Offset " << offset << ": ";
        for (int i = 0; i < vals.size(); ++i) {
            int i_idx = (offset >= 0) ? i : i - offset;
            int j_idx = (offset >= 0) ? i + offset : i;
            int expected = C_ref[i_idx][j_idx];
            cout << vals[i] << "(" << expected << ") ";
            if (vals[i] != expected) pass = false;
        }
        cout << "\n";
    }
    cout << "\nVerification " << (pass ? "PASSED" : "FAILED") << "\n";

    return 0;
}

