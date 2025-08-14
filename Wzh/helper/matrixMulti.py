import numpy as np
import sys
import os

def multiply_ghz_gates(
    qubit,
    num_gates,
    array_name="arr_0",
    save_to_txt=None,
    save_intermediate_txt=True
):
    """
    Multiply all gates sequentially for a given qubit count.

    Parameters:
        qubit (int): Number of qubits (determines folder path).
        num_gates (int): Number of gate files to process.
        array_name (str): Name of array inside each npz file.
        save_to_npy (str): Optional .npy filename to save final result.
        save_to_txt (str): Optional .txt filename to save as plain text (real part only).
        save_intermediate_txt (bool): Whether to save intermediate results to txt.

    Returns:
        np.ndarray: Final multiplied matrix.
    """

    # Build file paths
    files = [
        f"/mnt/beegfs/ysu34/supermarq/ghz/{qubit}/gate_{i}.npz"
        for i in range(1, num_gates + 1)
    ]

    if not files:
        raise ValueError("No gate files to process.")

    print("Files to process:")
    for f in files:
        print("  ", f)

    # Ensure output directory exists
    os.makedirs("./outputs", exist_ok=True)

    # Load the first matrix
    result = np.load(files[0])[array_name]
    print(f"Loaded {files[0]} shape {result.shape}")

    # Optionally save the first matrix as "step 1"
    if save_intermediate_txt:
        save_path = f"/mnt/beegfs/ysu34/{qubit}/intermediate_ghz_result_step_1.txt"
        _save_real_txt(result, save_path)
        print(f"Saved intermediate step 1 to {save_path}")

    # Sequential multiplication
    for i, path in enumerate(files[1:], start=2):
        mat = np.load(path)[array_name]
        print(f"Multiplying with {path} shape {mat.shape}")
        result = np.dot(result, mat)

        if save_intermediate_txt:
            save_path = f"/mnt/beegfs/ysu34/{qubit}/intermediate_ghz_result_step_{i}.txt"
            _save_real_txt(result, save_path)
            print(f"Saved intermediate step {i} to {save_path}")

    # Save .npy if requested
    # if save_to_npy:
    #     np.save(save_to_npy, result)
    #     print(f"Saved .npy result to {save_to_npy}")

    # Save .txt with real part only
    if save_to_txt:
        _save_real_txt(result, save_to_txt)
        print(f"Saved real-part text result to {save_to_txt}")

    return result

def _save_real_txt(array, filepath):
    """Helper to save only the real part to txt."""
    real_array = np.real(array)
    with open(filepath, "w") as f:
        for row in real_array:
            f.write(" ".join(f"{val:.6f}" for val in row))
            f.write("\n")

if __name__ == "__main__":
    qubit = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    num_gates = qubit  # or set explicitly if needed

    final_result = multiply_ghz_gates(
        qubit,
        num_gates,
        save_to_txt=f"/mnt/beegfs/ysu34/{qubit}/final_ghz_result.txt",
        save_intermediate_txt=True
    )

    print("Final result shape:", final_result.shape)
