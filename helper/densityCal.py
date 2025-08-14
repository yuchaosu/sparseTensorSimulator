import re

def compute_diagonal_density(diagonals, N):
    """Compute total number of elements if diagonals are dense."""
    count = 0
    for d in diagonals:
        if d >= 0:
            count += N - d
        else:
            count += N + d
    return count

def main():
    input_file = "ghz14_feature.txt"
    output_file = input_file

    with open(input_file, "r") as f:
        lines = f.readlines()

    # Extract N from first line
    first_line = lines[0].strip()
    match = re.search(r"chained multiplication of (\d+) matrices", first_line, re.IGNORECASE)
    if match:
        power = int(match.group(1))
        N = 2 ** power
        print(f"Detected matrix size: {N} x {N}")
    else:
        raise ValueError("Could not parse matrix size from the first line.")

    total_elements = N * N

    new_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]
        new_lines.append(line)

        # Detect start of a multiplication block
        if line.startswith("Multiplying matrix_output_"):
            # Look ahead to find the diagonals
            diagonals_line_idx = None

            for j in range(i + 1, min(i + 10, len(lines))):
                if lines[j].startswith("C_offsets after"):
                    diagonals_line_idx = j
                if lines[j].startswith("Cache stats:"):
                    break  # Reached end of relevant block

            if diagonals_line_idx is not None:
                diag_text = lines[diagonals_line_idx].split(":")[1]
                diag_strs = diag_text.strip().split()
                diagonals = [int(d) for d in diag_strs]

                nonzero_count = compute_diagonal_density(diagonals, N)
                density = nonzero_count / total_elements * 100
                diagonal_count = len(diagonals)

                density_line = f"C_density:{density:.4f}% \n"
                count_line = f"C_diagonal_count:{diagonal_count} \n"

                # Insert or replace the C_density line
                k = diagonals_line_idx + 1
                if k < len(lines) and lines[k].strip().startswith("C_density:"):
                    # Skip the old density line
                    i = k
                    new_lines.append(density_line)
                    new_lines.append(count_line)
                else:
                    new_lines.append(density_line)
                    new_lines.append(count_line)

        i += 1

    with open(output_file, "w") as f:
        f.writelines(new_lines)

    print(f"Done. Recomputed densities saved to '{output_file}'.")

if __name__ == "__main__":
    main()
