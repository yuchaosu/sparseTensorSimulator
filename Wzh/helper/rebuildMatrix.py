import sys

# for i in range(1, 11):
#     input_file = '/mnt/beegfs/ysu34/10/data/matrix_output_'+ str(i) +'.txt'

#     # Read lines from file
#     with open(input_file, "r") as f:
#         lines = [line.strip() for line in f if line.strip()]

#     # Parse entries
#     entries = {}
#     for line in lines:
#         parts = line.split(':')
#         idx = parts[0].strip()
#         val = float(parts[1].strip())
#         rowcol = idx.strip('()').split(',')
#         row = int(rowcol[0])
#         col = int(rowcol[1])
#         entries[(row, col)] = val

#     # Determine default size
#     max_row = max(r for r, _ in entries.keys())
#     max_col = max(c for _, c in entries.keys())

#     size = 1024

#     # Create matrix
#     matrix = [[0.0 for _ in range(size)] for _ in range(size)]

#     # Fill entries
#     for (r, c), v in entries.items():
#         if r < size and c < size:
#             matrix[r][c] = v
#         else:
#             print(f"Warning: ({r},{c}) outside size {size} ignored.")

#     # Write to output file
#     with open("/mnt/beegfs/ysu34/10/data/matrix" +  str(i) +".txt", "w") as f:
#         for row in matrix:
#             line = " ".join(f"{v:.6f}" for v in row)
#             f.write(line + "\n")

#     print(f"matrix.txt generated with shape {size}x{size}")

input_file = '/mnt/beegfs/ysu34/hamlib/maxcut/H_array_complbipart-n-12_a-6_b-6_sparse.txt'

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

size = 4096

# Create matrix
matrix = [[0.0 for _ in range(size)] for _ in range(size)]

# Fill entries
for (r, c), v in entries.items():
    if r < size and c < size:
        matrix[r][c] = v
    else:
        print(f"Warning: ({r},{c}) outside size {size} ignored.")

# Write to output file
with open("/mnt/beegfs/ysu34/hamlib/maxcut/data/matrix12_1.txt", "w") as f:
    for row in matrix:
        line = " ".join(f"{v:.6f}" for v in row)
        f.write(line + "\n")

print(f"matrix.txt generated with shape {size}x{size}")