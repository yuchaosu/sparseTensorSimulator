import numpy as np
import sys

def multiply_ghz_gates(
    qubit,
    num_gates,
    array_name="arr_0",
    save_to_npy=None,
    save_to_txt=None
):
    """
    Multiply all gates sequentially for a given qubit count.

    Parameters:
        qubit (int): Number of qubits (determines folder path).
        num_gates (int): Number of gate files to process.
        array_name (str): Name of array inside each npz file.
        save_to_npy (str): Optional .npy filename to save final result.
        save_to_txt (str): Optional .txt filename to save as plain text (real part only).

    Returns:
        np.ndarray: Final multiplied matrix.
    """

    # Build file paths
    files = [
        f"../with_h_data/supermarq/ghz/{qubit}/gate_{i}.npz"
        for i in range(1, num_gates + 1)
    ]

    if not files:
        raise ValueError("No gate files to process.")

    print("Files to process:")
    for f in files:
        print("  ", f)

    # Load the first matrix
    result = np.load(files[0])[array_name]
    print(f"Loaded {files[0]} shape {result.shape}")

    # Sequential multiplication
    for path in files[1:]:
        mat = np.load(path)[array_name]
        print(f"Multiplying with {path} shape {mat.shape}")
        result = np.dot(result, mat)

    # Save .npy if requested
    if save_to_npy:
        np.save(save_to_npy, result)
        print(f"Saved .npy result to {save_to_npy}")

    # Save .txt with real part only
    if save_to_txt:
        real_result = np.real(result)
        with open(save_to_txt, "w") as f:
            for row in real_result:
                f.write(" ".join(f"{val:.6f}" for val in row))
                f.write("\n")
        print(f"Saved real-part text result to {save_to_txt}")

    return result
if __name__ == "__main__":
    qubit = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    num_gates = qubit  # or set explicitly if needed

    final_result = multiply_ghz_gates(
        qubit,
        num_gates,
        save_to_npy=f"./outputs/final_ghz_result_{qubit}.npy",
        save_to_txt=f"./outputs/final_ghz_result_{qubit}.txt"
    )

    print("Final result shape:", final_result.shape)
    #print("First few entries:\n", final_result[:5, :5])
