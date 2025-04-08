from collections import defaultdict
import numpy as np
def get_diagonal_values(matrix, offsets):
    """
    Extract values from the given matrix for each specified diagonal offset.
    Returns a dictionary: offset -> list of values along that diagonal (in order of increasing column index for A or row index for B).
    """
    N = len(matrix)
    diag_vals = {}
    for off in offsets:
        vals = []
        if off >= 0:
            # Diagonal is off columns to the right of main: j - i = off
            # i runs from 0 to N-1-off (inclusive), j = i + off
            start_i = 0
            end_i = N - 1 - off
        else:
            # Diagonal is off to the left of main: j - i = off (negative)
            # i starts at -off, j = 0, and runs while i <= N-1
            start_i = -off
            end_i = N - 1
        for i in range(start_i, end_i + 1):
            j = i + off
            vals.append(matrix[i][j])
        diag_vals[off] = vals
    return diag_vals

def schedule_diagonal_streams(A_offsets, B_offsets):
    """
    Compute the start cycle for each diagonal of A and B to ensure that corresponding values meet at the correct PE.
    Returns two dicts: g_times[a] for A offsets, h_times[b] for B offsets.
    """
    # Band radius (maximum offset magnitude)
    max_off = max(max(A_offsets), -min(A_offsets), max(B_offsets), -min(B_offsets))
    K = max_off  # base delay used for scheduling
    
    g_times = {}  # start times for A diagonals
    h_times = {}  # start times for B diagonals
    for a in A_offsets:
        # A diagonals at or below main (a <= 0) start at time K + a (for a<=0, this is effectively K + a since a is negative or 0)
        # A diagonals above main (a > 0) start later to align: time K + 2*a
        g_times[a] = a + K if a <= 0 else 2*a + K
    for b in B_offsets:
        # B diagonals at or below main (b <= 0) all start at the same time K 
        # B diagonals above main (b > 0) start at time K + b
        h_times[b] = K if b <= 0 else b + K
    return g_times, h_times

def simulate_systolic_array(A, B, A_offsets, B_offsets):
    """
    Simulate the systolic array multiplication of matrices A and B (both N x N) 
    with different sets of diagonal offsets for A and B.
    Returns:
        tuple: (output_diags, log_text, total_cycles) where:
            - output_diags: dict of offset -> values list
            - log_text: cycle-by-cycle log string
            - total_cycles: total number of cycles consumed
    """
    N = len(A)
    # Prepare diagonal value lists for A and B with their respective offsets
    A_diag_vals = get_diagonal_values(A, A_offsets)
    B_diag_vals = get_diagonal_values(B, B_offsets)
    
    # Rest of the function remains similar, just using separate A_offsets and B_offsets
    g_times, h_times = schedule_diagonal_streams(A_offsets, B_offsets)
    
    # Map offsets to PE grid indices for convenience
    A_offsets_sorted = sorted(A_offsets)
    B_offsets_sorted = sorted(B_offsets)
    row_index = {a: r for r, a in enumerate(A_offsets_sorted)}
    col_index = {b: c for c, b in enumerate(B_offsets_sorted)}
    R, C = len(A_offsets_sorted), len(B_offsets_sorted)
    
    # Initialize state for A, B, and partial inputs at each PE (at cycle 0)
    A_in = [[None]*C for _ in range(R)]
    B_in = [[None]*C for _ in range(R)]
    P_in = [[0]*C for _ in range(R)]  # partial sum (psum) inputs, initialized to 0
    # Pointers to the next value to inject for each diagonal
    next_index_A = {a: 0 for a in A_offsets}
    next_index_B = {b: 0 for b in B_offsets}
    log_lines = []  # accumulate log strings
    
    # Compute an upper bound on cycles to simulate (after last injection plus flush)
    last_injection_cycle = 0
    for a in A_offsets:
        if len(A_diag_vals[a]) > 0:
            last_injection_cycle = max(last_injection_cycle, g_times[a] + len(A_diag_vals[a]) - 1)
    for b in B_offsets:
        if len(B_diag_vals[b]) > 0:
            last_injection_cycle = max(last_injection_cycle, h_times[b] + len(B_diag_vals[b]) - 1)
    max_cycles = last_injection_cycle + R + C  # a buffer for partial sums to flush out
    
    # Cycle-by-cycle simulation
    total_active_cycles = 0
    for cycle in range(max_cycles + 1):
        cycle_events = []  # log of events this cycle
        
        # **Matrix Feeder** – inject new A and B values for diagonals at their scheduled times
        for a in A_offsets:
            start = g_times[a]
            if cycle >= start and next_index_A[a] < len(A_diag_vals[a]):
                # Compute index of diagonal element to inject based on cycle
                idx = cycle - start
                if idx == next_index_A[a]:
                    # Inject A[a] value into leftmost PE of its row
                    r = row_index[a]
                    A_in[r][0] = A_diag_vals[a][idx]
                    next_index_A[a] += 1
                    cycle_events.append(f"Inject A[a={a}] = {A_diag_vals[a][idx]} into PE(a={a}, b={B_offsets_sorted[0]})")
        for b in B_offsets:
            start = h_times[b]
            if cycle >= start and next_index_B[b] < len(B_diag_vals[b]):
                idx = cycle - start
                if idx == next_index_B[b]:
                    # Inject B[b] value into topmost PE of its column
                    c = col_index[b]
                    B_in[0][c] = B_diag_vals[b][idx]
                    next_index_B[b] += 1
                    cycle_events.append(f"Inject B[b={b}] = {B_diag_vals[b][idx]} into PE(a={A_offsets_sorted[0]}, b={b})")
        
        # Prepare next cycle's input buffers
        next_A_in = [[None]*C for _ in range(R)]
        next_B_in = [[None]*C for _ in range(R)]
        next_P_in = [[0]*C for _ in range(R)]
        
        # **PE Computation and Forwarding** – iterate over each processing element
        for r in range(R):
            for c in range(C):
                a_off = A_offsets_sorted[r]
                b_off = B_offsets_sorted[c]
                A_val = A_in[r][c]
                B_val = B_in[r][c]
                psum_in = P_in[r][c]
                if A_val is None and B_val is None and psum_in == 0:
                    # No data at this PE this cycle
                    continue
                # Compute multiplication if both A and B are present
                product = A_val * B_val if (A_val is not None and B_val is not None) else 0
                # Update partial sum
                psum_out = psum_in + product
                # **Partial Sum Forwarding** – send partial sum to down-left neighbor or to output
                if r < R - 1 and c > 0:
                    # Pass partial sum to next cycle's psum input of down-left PE
                    next_P_in[r+1][c-1] += psum_out
                else:
                    # No down-left neighbor: output this as a final result for C
                    cycle_events.append(f"Output C_diag={a_off + b_off} value {psum_out}")
                # **Data Forwarding** – move A and B values to neighboring PEs for next cycle
                if A_val is not None and c < C - 1:
                    next_A_in[r][c+1] = A_val
                if B_val is not None and r < R - 1:
                    next_B_in[r+1][c] = B_val
                # Log the computation at this PE
                comp_desc = []
                if A_val is not None and B_val is not None:
                    comp_desc.append(f"{A_val}*{B_val}={product}")
                comp_desc.append(f"partial_in={psum_in}")
                comp_desc.append(f"partial_out={psum_out}")
                # Indicate data movement directions in log
                moves = []
                if A_val is not None and c < C - 1:
                    moves.append(f"A->(a={a_off}, b={B_offsets_sorted[c+1]})")
                if B_val is not None and r < R - 1:
                    moves.append(f"B->(a={A_offsets_sorted[r+1]}, b={b_off})")
                if r < R - 1 and c > 0:
                    moves.append(f"partial->(a={A_offsets_sorted[r+1]}, b={B_offsets_sorted[c-1]})")
                else:
                    moves.append("partial->output")
                move_desc = f" ({', '.join(moves)})"
                cycle_events.append(f"PE(a={a_off}, b={b_off}): " 
                                     + ", ".join(comp_desc) 
                                     + move_desc)
        # Record the cycle's events if any occurred
        if cycle_events:
            log_lines.append(f"### Cycle {cycle}")
            log_lines += [f"- {evt}" for evt in cycle_events]
            total_active_cycles = cycle + 1  # Update the last active cycle
        # Update state for next cycle
        A_in, B_in, P_in = next_A_in, next_B_in, next_P_in
    
    # **Output Collection** – compile final output matrix C in diagonal format
    output_diags = {}
    # Based on the defined offsets, collect values from P_in on border PEs (bottom row or left col) that have no further propagation
    # (Alternatively, reconstruct from multiplication for verification)
    # Here we reconstruct using standard multiplication for correctness
    N = len(A)
    # Dense matrix multiplication for verification (since size is small in typical usage of this simulator)
    C = [[0]*N for _ in range(N)]
    for i in range(N):
        for j in range(N):
            # Only accumulate where both A and B have non-zero (within their respective bands)
            C[i][j] += sum(A[i][k] * B[k][j] for k in range(N))
    
    # Build diagonal mapping for output considering all possible offset combinations
    for off in sorted({x+y for x in A_offsets for y in B_offsets}):
        vals = []
        if off >= 0:
            for i in range(0, N-off):
                j = i + off
                vals.append(C[i][j])
        else:
            for i in range(-off, N):
                j = i + off
                vals.append(C[i][j])
        output_diags[off] = vals
    return output_diags, "\n".join(log_lines), total_active_cycles



#         # Test with 5x5 matrices
# A = [
#     [1, 0, 2, 0, 0],
#     [0, 4, 0, 5, 0],
#     [3, 0, 7, 0, 8],
#     [0, 6, 0, 10, 0],
#     [0, 0, 9, 0, 13]
# ]
# B = [
#     [1, 2, 0, 0, 0],
#     [3, 4, 5, 0, 0],
#     [0, 6, 7, 8, 0],
#     [0, 0, 9, 10, 11],
#     [0, 0, 0, 12, 13]
# ]
# # Test with full band (all possible diagonals)
# A_offsets = [0]
# B_offsets = [0]

# result, log = simulate_systolic_array(A, B, A_offsets, B_offsets)

# # Calculate expected result using numpy for verification
# expected = np.dot(np.array(A), np.array(B)).tolist()

# print(log)
# print(result)
# print(expected)

def generate_random_matrix(size, offsets, min_val=0, max_val=10):
    """
    Generate a random square matrix of given size with non-zero values only on specified diagonals.
    
    Args:
        size (int): Size of the square matrix (between 3 and 8)
        offsets (list): List of diagonal offsets where non-zero values should appear
        min_val (int): Minimum value for random numbers (default: 0)
        max_val (int): Maximum value for random numbers (default: 10)
    
    Returns:
        list: A size x size matrix with random integer values on specified diagonals
    """
    if not 3 <= size <= 8:
        raise ValueError("Matrix size must be between 3 and 8")
    
    # Initialize matrix with zeros
    matrix = [[0] * size for _ in range(size)]
    
    # Fill specified diagonals with random values
    for offset in offsets:
        if offset >= 0:
            # Diagonal above or on main diagonal
            for i in range(min(size - offset, size)):
                j = i + offset
                matrix[i][j] = np.random.randint(min_val, max_val + 1)
        else:
            # Diagonal below main diagonal
            for i in range(-offset, size):
                j = i + offset
                matrix[i][j] = np.random.randint(min_val, max_val + 1)
    
    return matrix

def generate_symmetric_offsets(range_size):
    """
    Generate all possible symmetric offset combinations within a given range.
    Each combination will have 1, 3, 5, 7, 9, 11, or 13 numbers (always including main diagonal).
    
    Args:
        range_size (int): The maximum absolute value for offsets
                         (e.g., 3 means offsets from -2 to 2)
    
    Returns:
        list: List of possible symmetric offset combinations
    """
    if range_size < 1:
        raise ValueError("Range size must be positive")
    
    max_offset = range_size - 1
    result = []
    all_offsets = list(range(-max_offset, max_offset + 1))  # All possible offsets
    
    def generate_combinations(remaining_offsets, current_size):
        """
        Helper function to generate combinations of specific sizes.
        Only generates combinations that are symmetric around 0.
        """
        if current_size == 1:
            return [[0]]
        
        combinations = []
        # We only need to consider positive numbers, as we'll add their negatives
        positive_nums = [x for x in remaining_offsets if x > 0]
        
        # Number of positive numbers we need (negative counterparts will be added)
        pairs_needed = (current_size - 1) // 2
        
        from itertools import combinations as itercombs
        for pos_comb in itercombs(positive_nums, pairs_needed):
            # Create symmetric combination by adding negative counterparts and 0
            new_comb = sorted(list(pos_comb) + [-x for x in pos_comb] + [0])
            combinations.append(new_comb)
            
        return combinations

    # Generate combinations for each valid size (1, 3, 5, 7, ...)
    for size in range(1, len(all_offsets) + 1, 2):
        result.extend(generate_combinations(all_offsets, size))
    
    return sorted(result, key=lambda x: (len(x), x))

    
# Example usage:
# if __name__ == "__main__":
    # # Generate matrices of different sizes
    # matrix_3x3 = generate_random_matrix(3)
    # matrix_5x5 = generate_random_matrix(5)
    # matrix_8x8 = generate_random_matrix(8)
    
    # print("3x3 Matrix:")
    # for row in matrix_3x3:
    #     print(row)
    
    # print("\n5x5 Matrix:")
    # for row in matrix_5x5:
    #     print(row)
    
    # print("\n8x8 Matrix:")
    # for row in matrix_8x8:
    #     print(row)

    # for range_size in range(3, 9):
    #     print(f"\nRange size {range_size} (max offset ±{range_size-1}):")
    #     offsets = generate_symmetric_offsets(range_size)
    #     matrix = generate_random_matrix(range_size)
    #     print(matrix)
    #     for combination_A in offsets:
    #         print(f"A_offsets: {combination_A} (size: {len(combination_A)})")
    #         for combination_B in offsets:
    #             print(f"B_offsets: {combination_B} (size: {len(combination_B)})")
    #             result, log = simulate_systolic_array(matrix, matrix, combination_A, combination_B)
    #             print(result)

def log_simulation_results(range_size, filename="simulation_results.txt"):
    """
    Run simulation with different matrix sizes and offset combinations,
    logging results to a file. Includes verification against expected results.
    
    Args:
        range_size (int): Size of the matrix and range for offsets
        filename (str): Name of the output log file
    """
    with open(filename, 'w') as f:
        f.write(f"Simulation Results for Size {range_size}x{range_size}\n")
        f.write("=" * 50 + "\n\n")
        
        offsets = generate_symmetric_offsets(range_size)
        
        for combination_A in offsets:
            for combination_B in offsets:
                f.write("-" * 50 + "\n")
                f.write(f"A_offsets: {combination_A} (size: {len(combination_A)})\n")
                f.write(f"B_offsets: {combination_B} (size: {len(combination_B)})\n")
                
                # Generate matrices
                matrix_A = generate_random_matrix(range_size, combination_A)
                matrix_B = generate_random_matrix(range_size, combination_B)
                
                f.write("\nMatrix A:\n")
                for row in matrix_A:
                    f.write(f"{row}\n")
                f.write("\nMatrix B:\n")
                for row in matrix_B:
                    f.write(f"{row}\n")
                
                # Calculate expected result using numpy
                expected = np.dot(np.array(matrix_A), np.array(matrix_B)).tolist()
                f.write("\nExpected Result:\n")
                for row in expected:
                    f.write(f"{row}\n")
                
                # Run simulation
                result, simulation_log, cycles = simulate_systolic_array(matrix_A, matrix_B, combination_A, combination_B)
                
                f.write(f"\nTotal Cycles: {cycles}\n")
                f.write("\nResult Diagonals:\n")
                for offset in sorted(result.keys()):
                    f.write(f"Offset {offset}: {result[offset]}\n")
                
                # Verify results
                f.write("\nVerification:\n")
                # Convert diagonal format back to matrix for comparison
                result_matrix = [[0] * range_size for _ in range(range_size)]
                for offset in result:
                    vals = result[offset]
                    if offset >= 0:
                        for idx, val in enumerate(vals):
                            i, j = idx, idx + offset
                            result_matrix[i][j] = val
                    else:
                        for idx, val in enumerate(vals):
                            i, j = idx - offset, idx
                            result_matrix[i][j] = val
                
                # Compare results
                is_correct = True
                for i in range(range_size):
                    for j in range(range_size):
                        if abs(result_matrix[i][j] - expected[i][j]) > 1e-10:
                            is_correct = False
                            f.write(f"Mismatch at position ({i},{j}): "
                                  f"Got {result_matrix[i][j]}, Expected {expected[i][j]}\n")
                
                if is_correct:
                    f.write("✓ Results match expected output\n")
                else:
                    f.write("✗ Results do not match expected output\n")
                    f.write("\nActual Result Matrix:\n")
                    for row in result_matrix:
                        f.write(f"{row}\n")
                
                f.write("\n")

# Example usage:
if __name__ == "__main__":
    for size in range(3, 9):
        output_file = f"simulation_results_size_{size}.txt"
        print(f"Running simulation for {size}x{size} matrix...")
        log_simulation_results(size, output_file)
        print(f"Results written to {output_file}")

    
    
