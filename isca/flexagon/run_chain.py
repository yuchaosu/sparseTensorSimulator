#!/usr/bin/env python3.11
"""Faithful chained-power Flexagon (SST-STONNE) driver.

The mesh baselines (DIAMOND/TPU/Trapezoid) with -iter=K compute the real Taylor chain:
K SpMSpM multiplies H^k @ H for k=1..K, the LEFT operand densifying each step
(main/diamond.cpp:969-984, `convOnGrid(current_diag, H_diag, ...)` with current_diag <- C).

run_case.py's --dia mode instead hardcodes A=B=H (one non-densifying multiply), so it is NOT
comparable. This driver fixes that for Flexagon: it forms H^1..H^K with scipy (exactly like
analytical/real_oom_scipy.py's `Hp = Hp.dot(H)`), feeds A=H^k / B=H into STONNE at each step,
and SUMS the cycle counts. Cycles depend on the sparsity PATTERN only, so STONNE values are 1.0
(run_case convention); the pattern IS the real H^k pattern (real-valued chain, exact
cancellations dropped — matching DIAMOND and real_oom_scipy).

Emits ONE CSV row:
  impl,workload,q,K,class,pe,cycles_total,per_step_cycles,per_step_nnz,status,sec
per_step_* are ';'-joined. status: OK | OOM_HOST(H^p) | CRASH(H^p) | TIMEOUT(H^p) | ERR:...
"""
import argparse, os, sys, time, shutil, subprocess
import numpy as np
import scipy.sparse as sp

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_case as rc                                   # reuse resolve_impl/write_config/parse_stats/read_dia_pattern

STONNE_DIM_CEIL = 65536                                 # STONNE hard-crashes at matrix dim >= 2^16 (q>=16)


def load_base_H(dia_path, csr_path):
    """Return base H as a CSR pattern matrix (values 1.0). Prefer the real .csr.npz (so the
    chained powers carry real cancellations); fall back to the .txt 0/1 pattern."""
    if csr_path and os.path.exists(csr_path):
        H = sp.load_npz(csr_path).tocsr()
        H.eliminate_zeros()
        return H, "csr.npz(real)"
    n, rows, _cols = rc.read_dia_pattern(dia_path)
    indptr = [0]; indices = []
    for r in rows:
        indices.extend(r); indptr.append(len(indices))
    H = sp.csr_matrix((np.ones(len(indices)), np.array(indices, dtype=np.int64),
                       np.array(indptr, dtype=np.int64)), shape=(n, n))
    return H, "txt(pattern)"


def write_operands(work, impl, A, H):
    """Write STONNE CSR/CSC operand files for A@H, matching run_case.build_dia_case layout:
    op -> A col-major (CSC), B row-major (CSR); gustavson -> A CSR, B CSR. B is always H (CSR)."""
    pfx = "outerproduct_gemm" if impl == "op" else "gustavsons_gemm"
    if impl == "op":
        Ac = A.tocsc(); pA, iA = Ac.indptr, Ac.indices
    else:
        Ar = A.tocsr(); pA, iA = Ar.indptr, Ar.indices
    pB, iB = H.indptr, H.indices
    nnzA, nnzB = int(len(iA)), int(len(iB))
    W = lambda name, seq: open(os.path.join(work, name), "w").write(",".join(map(str, (int(x) for x in seq))))
    W(f"{pfx}_rowpointerA.in", pA); W(f"{pfx}_colpointerA.in", iA)
    W(f"{pfx}_rowpointerB.in", pB); W(f"{pfx}_colpointerB.in", iB)
    open(os.path.join(work, f"{pfx}_mem.ini"), "w").write(
        ",".join([str(rc.FLOAT_ONE)] * (nnzA + nnzB) + ["0"]))
    return pfx, nnzA, nnzB


def run_one_step(args, sst_bin, testdir, step_dir, A, H, n):
    """Build operands for A@H, run one STONNE sim, return (cycles, status). status in
    {OK, CRASH, TIMEOUT, OOM, ERR:...}."""
    os.makedirs(step_dir, exist_ok=True)
    cfg, kernel = rc.resolve_impl(args.impl, args.mses)
    shutil.copy(os.path.join(testdir, cfg), os.path.join(step_dir, cfg))
    pfx, nnzA, nnzB = write_operands(step_dir, args.impl, A, H)
    c = dict(M=n, K=n, N=n, nnzA=nnzA, nnzB=nnzB, cfg=cfg, kernel=kernel,
             addr_a=0, addr_b=nnzA * 4, addr_c=nnzA * 4 + nnzB * 4, layout="csr", pfx=pfx)
    rc.write_config(args, step_dir, c)                  # writes run.py (uses args.mem, args.hbm4_config)
    cmd = f"cd {step_dir} && {sst_bin} run.py"
    t0 = time.time()
    try:
        p = subprocess.run(["bash", "-lc", cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=args.timeout_s)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT", time.time() - t0
    out = p.stdout or ""
    dt = time.time() - t0
    # STONNE dumps a huge (mem_size) result.out per step; drop it immediately to spare /tmp.
    try:
        os.remove(os.path.join(step_dir, "result.out"))
    except OSError:
        pass
    if p.returncode == 0 and "Simulation is complete" in out:
        cyc, _ops = rc.parse_stats(step_dir)
        return (cyc, "OK", dt) if cyc is not None else (None, "ERR:no_cycles", dt)
    if "bad_alloc" in out or "out of memory" in out.lower() or p.returncode == 137:
        return None, "OOM", dt
    tail = (out.strip().splitlines() or [""])[-1][:50].replace(",", ";")
    return None, "CRASH:" + tail, dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--impl", required=True, choices=["op", "gustavson"])
    ap.add_argument("--dia", required=True, help="DIA .txt workload path (base H)")
    ap.add_argument("--csr", default=None, help="base H .csr.npz (preferred); else derived from --dia")
    ap.add_argument("--workload", required=True)
    ap.add_argument("--q", type=int, required=True)
    ap.add_argument("--K", type=int, required=True)
    ap.add_argument("--cls", default="")
    ap.add_argument("--mses", type=int, default=256)
    ap.add_argument("--mem", default="ramulator2", choices=["ramulator2", "simple"])
    ap.add_argument("--repo", default="/home/ysu34/DIAMOND/diamond")
    ap.add_argument("--timeout-s", type=int, default=79200)
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()
    args.hbm4_config = f"{args.repo}/config/hbm4_sota.yaml"
    flex = f"{args.repo}/baselines/flexagon"
    testdir = f"{flex}/sst-elements-with-stonne/src/sst/elements/sstStonne/tests"
    sst_bin = f"{flex}/install/sst-core/bin/sst"
    work = args.workdir or f"/tmp/flexchain_{args.impl}_{args.workload}_{os.getpid()}"
    os.makedirs(work, exist_ok=True)
    pe = f"{int(round(args.mses**0.5))}x{int(round(args.mses**0.5))}"  # 256 -> 16x16

    def emit(total, steps_cyc, steps_nnz, status, sec):
        print(",".join(map(str, [
            args.impl, args.workload, args.q, args.K, args.cls, pe,
            total if total != "" else "",
            ";".join(str(x) for x in steps_cyc),
            ";".join(str(x) for x in steps_nnz),
            status, f"{sec:.1f}"])))

    t_all = time.time()
    try:
        H, src = load_base_H(args.dia, args.csr)
    except Exception as e:
        emit("", [], [], "ERR:load:" + str(e)[:40].replace(",", ";"), time.time() - t_all)
        return
    n = H.shape[0]
    if n >= STONNE_DIM_CEIL:                             # q>=16: STONNE dim ceiling -> cannot run
        emit("", [], [], f"CRASH(dim{n}>=65536,q{args.q})", time.time() - t_all)
        shutil.rmtree(work, ignore_errors=True)
        return

    steps_cyc, steps_nnz = [], []
    P = H.copy()                                         # H^1
    total = 0
    status = "OK"
    for k in range(1, args.K + 1):
        steps_nnz.append(int(P.nnz))
        cyc, st, _dt = run_one_step(args, sst_bin, testdir, os.path.join(work, f"s{k}"), P, H, n)
        if st != "OK":
            status = f"{st}(H^{k})"; steps_cyc.append("")
            break
        steps_cyc.append(cyc); total += cyc
        shutil.rmtree(os.path.join(work, f"s{k}"), ignore_errors=True)   # free step dir after parse
        if k < args.K:                                   # densify to H^(k+1) for next multiply's A
            try:
                P = (P.dot(H)).tocsr(); P.eliminate_zeros()
            except MemoryError:
                status = f"OOM_HOST(H^{k+1})"; break
    emit(total if status == "OK" else "", steps_cyc, steps_nnz, status, time.time() - t_all)
    if args.workdir is None:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
