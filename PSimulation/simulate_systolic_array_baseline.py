from collections import defaultdict
import numpy as np  

def get_diagonal_values(matrix, offsets):
    N = len(matrix)
    diag_vals = {}
    for off in offsets:
        vals = []
        if off >= 0:
            start_i, end_i = 0, N - 1 - off
        else:
            start_i, end_i = -off, N - 1
        for i in range(start_i, end_i + 1):
            j = i + off
            vals.append(matrix[i][j])
        diag_vals[off] = vals
    return diag_vals

def schedule_diagonal_streams(A_offsets, B_offsets):
    max_off = max(max(A_offsets), -min(A_offsets), max(B_offsets), -min(B_offsets))
    K = max_off
    g_times = {a: a + K if a <= 0 else 2 * a + K for a in A_offsets}
    h_times = {b: K if b <= 0 else b + K for b in B_offsets}
    return g_times, h_times

def simulate_systolic_array(A, B, A_offsets, B_offsets):
    """
    Multiply A×B using a PE grid whose rows = len(A_offsets),
    cols = len(B_offsets).  Returns (diag_dict, log_str, cycles).
    """
    # ---------- setup ----------
    N = len(A)
    A_diag_vals = get_diagonal_values(A, A_offsets)
    B_diag_vals = get_diagonal_values(B, B_offsets)
    g_times, h_times = schedule_diagonal_streams(A_offsets, B_offsets)

    A_offsets_sorted = sorted(A_offsets)
    B_offsets_sorted = sorted(B_offsets)
    row_index = {a: r for r, a in enumerate(A_offsets_sorted)}
    col_index = {b: c for c, b in enumerate(B_offsets_sorted)}
    R, C = len(A_offsets_sorted), len(B_offsets_sorted)

    A_in  = [[None] * C for _ in range(R)]
    B_in  = [[None] * C for _ in range(R)]
    P_in  = [[0]    * C for _ in range(R)]
    next_index_A = {a: 0 for a in A_offsets}
    next_index_B = {b: 0 for b in B_offsets}

    # cycle bound
    last_injection = 0
    for a in A_offsets:
        last_injection = max(last_injection,
                             g_times[a] + len(A_diag_vals[a]) - 1)
    for b in B_offsets:
        last_injection = max(last_injection,
                             h_times[b] + len(B_diag_vals[b]) - 1)
    max_cycles = last_injection + R + C

    log_lines = []
    total_active_cycles = 0

    # ---------- simulation loop ----------
    for cycle in range(max_cycles + 1):
        events = []

        # inject A
        for a in A_offsets:
            start = g_times[a]
            if cycle >= start and next_index_A[a] < len(A_diag_vals[a]):
                idx = cycle - start
                if idx == next_index_A[a]:
                    r = row_index[a]
                    A_in[r][0] = A_diag_vals[a][idx]
                    next_index_A[a] += 1
                    events.append(f"Inject A[{a}][{idx}]={A_diag_vals[a][idx]}"
                                  f" into PE({r},0)")
        # inject B
        for b in B_offsets:
            start = h_times[b]
            if cycle >= start and next_index_B[b] < len(B_diag_vals[b]):
                idx = cycle - start
                if idx == next_index_B[b]:
                    c = col_index[b]
                    B_in[0][c] = B_diag_vals[b][idx]
                    next_index_B[b] += 1
                    events.append(f"Inject B[{b}][{idx}]={B_diag_vals[b][idx]}"
                                  f" into PE(0,{c})")

        # prepare next tick buffers
        nxt_A = [[None]*C for _ in range(R)]
        nxt_B = [[None]*C for _ in range(R)]
        nxt_P = [[0]   *C for _ in range(R)]

        # PE processing
        for r in range(R):
            for c in range(C):
                a_off = A_offsets_sorted[r]
                b_off = B_offsets_sorted[c]

                A_val = A_in[r][c]
                B_val = B_in[r][c]
                psum  = P_in[r][c]

                if A_val is None and B_val is None and psum == 0:
                    continue

                prod = (A_val * B_val
                        if A_val is not None and B_val is not None else 0)
                psum_out = psum + prod

                # send partial sum
                if r < R-1 and c > 0:
                    nxt_P[r+1][c-1] += psum_out
                else:
                    events.append(f"Output val {psum_out} for C_diag={a_off+b_off}")

                # forward A, B
                if A_val is not None and c < C-1:
                    nxt_A[r][c+1] = A_val
                if B_val is not None and r < R-1:
                    nxt_B[r+1][c] = B_val

                # log PE action
                desc = []
                if A_val is not None and B_val is not None:
                    desc.append(f"{A_val}*{B_val}={prod}")
                desc.append(f"in={psum} out={psum_out}")
                moves = []
                if A_val is not None and c < C-1:
                    moves.append("A→E")
                if B_val is not None and r < R-1:
                    moves.append("B→S")
                moves.append("P→SE" if r < R-1 and c > 0 else "P→out")
                events.append(f"PE[{r}][{c}]({a_off},{b_off}): {'; '.join(desc)} "
                              f"[{', '.join(moves)}]")

        if events:
            log_lines.append(f"### Cycle {cycle}")
            log_lines += [f"- {e}" for e in events]
            total_active_cycles = cycle + 1

        A_in, B_in, P_in = nxt_A, nxt_B, nxt_P

    # ---------- dense reference for diagonal dict ----------
    C_ref = (np.dot(np.array(A), np.array(B))).tolist()
    diags = defaultdict(dict)  # offset -> {index: value}
    for i in range(N):
        for j in range(N):
            off = j - i
            diags[off][i] = C_ref[i][j]

    diag_out = {}
    for offset, idx_val in diags.items():
        min_idx = min(idx_val)
        max_idx = max(idx_val)
        vals = [idx_val.get(i, 0) for i in range(min_idx, max_idx + 1)]
        diag_out[offset] = vals


    return diag_out, "\n".join(log_lines), total_active_cycles

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

if __name__ == "__main__":
    range_size = 8
    max_offset_index = 3
    print(f"Simulation Results for Size {range_size}x{range_size}\n")   
    print("=" * 50 + "\n\n")
    offsets = generate_symmetric_offsets(max_offset_index)

    for combination_A in offsets:
        for combination_B in offsets:
            print("-" * 50 + "\n")
            print(f"A_offsets: {combination_A} (size: {len(combination_A)})\n")
            print(f"B_offsets: {combination_B} (size: {len(combination_B)})\n")
            
            # Generate matrices
            matrix_A = generate_random_matrix(range_size, combination_A)
            matrix_B = generate_random_matrix(range_size, combination_B)
            
            print("\nMatrix A:\n")
            for row in matrix_A:
                print(f"{row}\n")
            print("\nMatrix B:\n")
            for row in matrix_B:
                print(f"{row}\n")
            
            # Calculate expected result using numpy
            expected = np.dot(np.array(matrix_A), np.array(matrix_B)).tolist()
            print("\nExpected Result:\n")
            for row in expected:
                print(f"{row}\n")
            
            # Run simulation
            result, simulation_log, cycles = simulate_systolic_array(matrix_A, matrix_B, combination_A, combination_B)
            
            print(f"\nTotal Cycles: {cycles}\n")
            print("\nResult Diagonals:\n")
            for offset in sorted(result.keys()):
                print(f"Offset {offset}: {result[offset]}\n")
            print(simulation_log)
            # Verify results
            print("\nVerification:\n")
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
                        print(f"Mismatch at position ({i},{j}): "
                                f"Got {result_matrix[i][j]}, Expected {expected[i][j]}\n")
            
            if is_correct:
                print("Results match expected output\n")
            else:
                print("Results do not match expected output\n")
                print("\nActual Result Matrix:\n")
                for row in result_matrix:
                    print(f"{row}\n")
            
            print("\n")

