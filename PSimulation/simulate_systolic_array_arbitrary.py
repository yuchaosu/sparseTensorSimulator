from collections import defaultdict
import numpy as np

def _chunked(lst, size):
    for i in range(0, len(lst), size):
        yield lst[i:i+size]

def _mask_matrix(mat, keep_offsets):
    N = len(mat)
    out = [[0]*N for _ in range(N)]
    for off in keep_offsets:
        if off >= 0:
            for i in range(N-off):
                j = i + off
                out[i][j] = mat[i][j]
        else:
            for i in range(-off, N):
                j = i + off
                out[i][j] = mat[i][j]
    return out

def _dense_to_diagonals(C):
    N = len(C)
    d = defaultdict(list)
    for i in range(N):
        for j in range(N):
            d[j-i].append(C[i][j])
    return {k: d[k] for k in sorted(d)}

from simulate_systolic_array_baseline import simulate_systolic_array, generate_symmetric_offsets, generate_random_matrix

def simulate_systolic_array_arbitrary_grid(A, B, A_offsets, B_offsets, grid_rows, grid_cols):
    if grid_rows >= len(A_offsets) and grid_cols >= len(B_offsets):
        return simulate_systolic_array(A, B, A_offsets, B_offsets)
    N = len(A)
    C_acc = [[0]*N for _ in range(N)]
    total_cycles = 0
    merged_log = []

    for A_chunk in _chunked(A_offsets, grid_rows):
        A_masked = _mask_matrix(A, A_chunk)
        for B_chunk in _chunked(B_offsets, grid_cols):
            B_masked = _mask_matrix(B, B_chunk)
            diag_part, log_part, cycles = simulate_systolic_array(
                A_masked, B_masked, A_chunk, B_chunk
            )

            # accumulate dense partial
            dense_part = [[0]*N for _ in range(N)]
            for off, vals in diag_part.items():
                if off >= 0:
                    for k, v in enumerate(vals):
                        dense_part[k][k+off] += v
                else:
                    for k, v in enumerate(vals):
                        dense_part[k-off][k] += v
            for i in range(N):
                for j in range(N):
                    C_acc[i][j] += dense_part[i][j]

            merged_log.append(f"# --- tile A:{A_chunk}  B:{B_chunk} ---")
            merged_log.append(log_part)
            total_cycles += cycles          # sequential execution

    return _dense_to_diagonals(C_acc), "\n".join(merged_log), total_cycles


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
            
            grid_rows = 2
            grid_cols = 2
            # Run simulation
            result, simulation_log, cycles = simulate_systolic_array_arbitrary_grid(matrix_A, matrix_B, combination_A, combination_B, grid_rows, grid_cols)
            
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

