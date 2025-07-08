import sys


input_file = './outputs/8/matrix_output_2.txt'

# Read lines from file
with open(input_file, "r") as f:
    lines = [line.strip() for line in f if line.strip()]

# Parse entries
entries = {}
for line in lines:
    parts = line.split(':')
    idx = parts[0].strip()
    val = float(parts[1].strip())
    rowcol = idx.strip('()').split(',')
    row = int(rowcol[0])
    col = int(rowcol[1])
    entries[(row, col)] = val

# Determine default size
max_row = max(r for r, _ in entries.keys())
max_col = max(c for _, c in entries.keys())

size = 256

# Create matrix
matrix = [[0.0 for _ in range(size)] for _ in range(size)]

# Fill entries
for (r, c), v in entries.items():
    if r < size and c < size:
        matrix[r][c] = v
    else:
        print(f"Warning: ({r},{c}) outside size {size} ignored.")

# Write to output file
with open("./outputs/matrix1.txt", "w") as f:
    for row in matrix:
        line = " ".join(f"{v:.6f}" for v in row)
        f.write(line + "\n")

print(f"matrix.txt generated with shape {size}x{size}")
