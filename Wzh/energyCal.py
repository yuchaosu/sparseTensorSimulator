import sys
import re
import os
from collections import defaultdict

def main():
    if len(sys.argv) != 3:
        print("Usage: python script.py <energy_area_table_file> <counts_file>")
        sys.exit(1)

    table_file = sys.argv[1]
    counts_file = sys.argv[2]

    # 1. Read the table file
    op_params = {}
    with open(table_file, "r") as f:
        lines = f.readlines()
        for line in lines[1:]:  # skip header
            parts = re.split(r'\s+', line.strip())
            if len(parts) >= 4:
                op = parts[0].rstrip(":").lower()
                energy = float(parts[1])
                area = float(parts[2])
                static = float(parts[3])
                op_params[op] = {
                    "energy": energy,
                    "area": area,
                    "static": static
                }

    # 2. Read the counts file
    energy_counts = defaultdict(int)
    area_static_counts = defaultdict(int)
    separate_counts = defaultdict(int)

    with open(counts_file, "r") as f:
        lines = f.readlines()
        for line in lines:
            line = line.strip()
            if not line:
                continue
            m = re.match(r"(\w+):?\s*(\d+)", line)
            if not m:
                continue
            op_raw = m.group(1).lower()
            count = int(m.group(2))

            separate_counts[op_raw] += count

            if op_raw == "receives":
                energy_counts["sends"] += count
                area_static_counts["sends"] += count
            elif op_raw == "sends":
                energy_counts["sends"] += count
                area_static_counts["sends"] += count
            else:
                energy_counts[op_raw] += count
                area_static_counts[op_raw] += count

    # For area/static, count unique send-receive pairs
    if "sends" in area_static_counts:
        area_static_counts["sends"] = area_static_counts["sends"] // 2

    # 3. Calculate totals
    total_energy = 0
    total_area = 0
    total_static = 0

    report_lines = []

    report_lines.append("Per Operation Summary:\n")
    report_lines.append(f"{'Operation':<12} {'Count':>10} {'Energy':>15} {'Area':>15} {'Static':>15}\n")

    # For clarity, show sends/receives counts separately
    operations_list = sorted(op_params.keys())
    if "sends" not in operations_list:
        operations_list.append("sends")
    if "receives" not in operations_list:
        operations_list.append("receives")

    for op in operations_list:
        # For energy counts
        if op == "receives":
            e_count = separate_counts["receives"]
        else:
            e_count = energy_counts[op]

        # For area/static counts
        if op == "receives":
            a_count = 0  # receives don't contribute additional area/static
        else:
            a_count = area_static_counts[op]

        # Lookup parameters (receives re-use sends parameters for energy)
        param_op = "sends" if op == "receives" else op
        energy_per = op_params.get(param_op, {}).get("energy", 0.0)
        area_per = op_params.get(op, {}).get("area", 0.0)
        static_per = op_params.get(op, {}).get("static", 0.0)

        e = e_count * energy_per
        a = a_count * area_per
        s = a_count * static_per

        total_energy += e
        total_area += a
        total_static += s

        report_lines.append(f"{op:<12} {e_count:10d} {e:15.6f} {a:15.6f} {s:15.6f}\n")
    total_area /= 48
    report_lines.append("\nTOTALS:\n")
    report_lines.append(f"Total Energy: {total_energy:.6f}\n")
    report_lines.append(f"Total Area: {total_area:.6f}\n")
    report_lines.append(f"Total Static: {total_static:.6f}\n")

    # 4. Determine output file name
    base_name = os.path.splitext(counts_file)[0]
    output_filename = f"{base_name}.power"

    with open(output_filename, "w") as f:
        f.writelines(report_lines)

    # Also print summary to console
    print(f"Calculation complete. Results saved to {output_filename}.\n")
    print(f"Total Energy: {total_energy:.6f}")
    print(f"Total Area: {total_area:.6f}")
    print(f"Total Static: {total_static:.6f}")

if __name__ == "__main__":
    main()
