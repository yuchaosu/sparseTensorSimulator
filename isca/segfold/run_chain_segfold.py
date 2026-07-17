#!/usr/bin/env python3
"""Faithful chained-power SegFold (csegfold SpMSpM) baseline driver.

Mirrors isca/flexagon/run_chain.py, but for SegFold (github.com/PolyArch/SegFold-AE):
the mesh baselines (DIAMOND/TPU/Trapezoid) with -iter=K compute the real Taylor chain
of K SpMSpM multiplies H^k @ H for k=1..K, the LEFT operand densifying each step
(main/diamond.cpp:969-984). This driver reproduces that for SegFold: it forms H^1..H^K
with scipy (exactly like analytical/real_oom_scipy.py's `Hp = Hp.dot(H)`), writes A=H^k
and B=H as Matrix Market .mtx, runs `csegfold --mtx-file A --mtx-file-b B` at each step,
and SUMS the reported cycle counts. Cycles depend on the sparsity PATTERN only, so values
are 1.0 (the pattern IS the real H^k pattern; real-valued chain, exact cancellations
dropped -- matching DIAMOND / real_oom_scipy / the Flexagon chain driver).

csegfold does A x B (--mtx-file / --mtx-file-b, main.cpp), array size from the config
(physical_pe_row_num/col_num = 16x16), and prints
  "Simulation completed successfully in <N> cycles"
plus a run_*_stats.json (sim.stats.cycle). HBM is modeled by Ramulator2 (HBM4 via the
config's dram_config_file), so these cycles are HBM-inclusive like Flexagon/Trapezoid.

Emits ONE CSV row (same schema as flex_*.csv so combine_16x16.py can read it):
  impl,workload,q,K,class,pe,cycles_total,per_step_cycles,per_step_nnz,status,sec
per_step_* are ';'-joined. status: OK | OOM_HOST(H^p) | CRASH(H^p) | TIMEOUT(H^p) | ERR:...
Unlike STONNE there is no 2^16 matrix-dim ceiling, so q>=16 can run (host memory permitting).
"""
import argparse, os, re, sys, time, shutil, subprocess, glob
import numpy as np
import scipy.sparse as sp
import scipy.io as sio

REPO = "/home/ysu34/DIAMOND/diamond"
CYC_RE = re.compile(r"completed successfully in\s+(\d+)\s+cycles")


def read_dia_pattern(dia_path):
    """Parse a DIA .txt ('N <n> D <d>' then '<off>: v v ...') into (n, csr_pattern).
    Values are set to 1.0 (pattern only -- cycles depend on structure, not values)."""
    with open(dia_path) as f:
        first = f.readline().split()
        n = int(first[1])
        rows, cols = [], []
        for line in f:
            c = line.find(":")
            if c < 0:
                continue
            off = int(line[:c])
            vals = line[c + 1:].split()
            length = n - abs(off)
            for k in range(min(length, len(vals))):
                if float(vals[k]) != 0.0:
                    r = k if off >= 0 else k - off
                    cc = k + off if off >= 0 else k
                    rows.append(r); cols.append(cc)
    H = sp.csr_matrix((np.ones(len(rows)), (rows, cols)), shape=(n, n))
    return n, H


def load_base_H(dia_path, csr_path):
    """Base H as a CSR pattern matrix (1.0 values). Prefer the real .csr.npz (carries the
    real cancellations of the chained powers); else derive the 0/1 pattern from the .txt."""
    if csr_path and os.path.exists(csr_path):
        H = sp.load_npz(csr_path).tocsr()
        H.eliminate_zeros()
        H.data[:] = 1.0
        return H, "csr.npz(real)"
    _n, H = read_dia_pattern(dia_path)
    return H, "txt(pattern)"


def run_one_step(bin_, config, step_dir, A, H, timeout_s):
    """Write A,B as .mtx and run one csegfold SpMSpM. Return (cycles, status, sec)."""
    os.makedirs(step_dir, exist_ok=True)
    a_mtx = os.path.join(step_dir, "A.mtx")
    b_mtx = os.path.join(step_dir, "B.mtx")
    # Matrix Market; coordinate/general. mmwrite needs COO. Values are 1.0 (pattern).
    sio.mmwrite(a_mtx, A.tocoo(), field="real", symmetry="general")
    sio.mmwrite(b_mtx, H.tocoo(), field="real", symmetry="general")
    cmd = [bin_, "--config", config, "--mtx-file", a_mtx, "--mtx-file-b", b_mtx,
           "--tmp-dir", step_dir]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT", time.time() - t0
    dt = time.time() - t0
    out = p.stdout or ""
    m = CYC_RE.search(out)
    if p.returncode == 0 and m:
        return int(m.group(1)), "OK", dt
    if "bad_alloc" in out or "out of memory" in out.lower() or p.returncode == 137:
        return None, "OOM", dt
    tail = (out.strip().splitlines() or [""])[-1][:50].replace(",", ";")
    return None, "CRASH:" + tail, dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--impl", default="segfold")
    ap.add_argument("--dia", required=True, help="DIA .txt workload path (base H)")
    ap.add_argument("--csr", default=None, help="base H .csr.npz (preferred); else from --dia")
    ap.add_argument("--workload", required=True)
    ap.add_argument("--q", type=int, required=True)
    ap.add_argument("--K", type=int, required=True)
    ap.add_argument("--cls", default="")
    ap.add_argument("--config", default=f"{REPO}/isca/segfold/segfold_16x16_hbm4.yaml")
    ap.add_argument("--bin", default=f"{REPO}/external/SegFold-AE/csegfold/build/csegfold")
    ap.add_argument("--timeout-s", type=int, default=7200)
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()

    pe = "16x16"   # config is physical_pe_row_num/col_num = 16
    work = args.workdir or f"/tmp/segfoldchain_{args.workload}_{os.getpid()}"
    os.makedirs(work, exist_ok=True)

    def emit(total, steps_cyc, steps_nnz, status, sec):
        print(",".join(map(str, [
            args.impl, args.workload, args.q, args.K, args.cls, pe,
            total if total != "" else "",
            ";".join(str(x) for x in steps_cyc),
            ";".join(str(x) for x in steps_nnz),
            status, f"{sec:.1f}"])))

    t_all = time.time()
    try:
        H, _src = load_base_H(args.dia, args.csr)
    except Exception as e:
        emit("", [], [], "ERR:load:" + str(e)[:40].replace(",", ";"), time.time() - t_all)
        return

    steps_cyc, steps_nnz = [], []
    P = H.copy()          # H^1
    total = 0
    status = "OK"
    for k in range(1, args.K + 1):
        steps_nnz.append(int(P.nnz))
        cyc, st, _dt = run_one_step(args.bin, args.config, os.path.join(work, f"s{k}"),
                                    P, H, args.timeout_s)
        shutil.rmtree(os.path.join(work, f"s{k}"), ignore_errors=True)
        if st != "OK":
            status = f"{st}(H^{k})"; steps_cyc.append(""); break
        steps_cyc.append(cyc); total += cyc
        if k < args.K:                          # densify to H^(k+1) for next multiply's A
            try:
                P = (P.dot(H)).tocsr(); P.eliminate_zeros(); P.data[:] = 1.0
            except MemoryError:
                status = f"OOM_HOST(H^{k+1})"; break
    emit(total if status == "OK" else "", steps_cyc, steps_nnz, status, time.time() - t_all)
    if args.workdir is None:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
