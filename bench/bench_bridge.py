#!/usr/bin/env python3
"""
bench_bridge.py — Python side of the Step 2.4 handoff measurement.

The C++ side (bench_cascade, section 3) times the fread load. This side times
what Python spends producing the payload, and compares three mechanisms on
identical data:

  1. tobytes + file write  — what scripts/infer_network.py actually does.
  2. pickle                — the naive baseline asked for.
  3. shared_memory         — what an actual zero-copy handoff would cost,
                             included so the "zero-copy" claim has a yardstick.

No claim is made that (3) is implemented in OptiRisk. It is not; this measures
what the project would have to beat to earn the word.
"""

import json
import os
import pickle
import statistics
import sys
import time
from multiprocessing import shared_memory

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")

N_NODES = 500
N_EDGES = 8192
ITERS = 200


def build_payload(seed=20260927):
    """Same shapes and dtypes scripts/infer_network.py writes."""
    rng = np.random.default_rng(seed)
    return {
        "risk_score": rng.random(N_NODES).astype(np.float32),
        "is_defaulted": np.zeros(N_NODES, dtype=np.uint8),
        "is_hero": np.zeros(N_NODES, dtype=np.uint8),
        "equities": rng.random(N_NODES) * 1e9,
        "real_estate": rng.random(N_NODES) * 1e9,
        "crypto": rng.random(N_NODES) * 1e9,
        "treasuries": rng.random(N_NODES) * 1e9,
        "corp_bonds": rng.random(N_NODES) * 1e9,
        "total_assets": rng.random(N_NODES) * 1e10,
        "liabilities": rng.random(N_NODES) * 1e10,
        "nav": rng.random(N_NODES) * 1e9,
        "credit_rating": rng.random(N_NODES).astype(np.float32),
        "sector_id": rng.integers(0, 10, N_NODES, dtype=np.uint32),
        "lat": rng.random(N_NODES).astype(np.float32),
        "lon": rng.random(N_NODES).astype(np.float32),
        "hub": rng.integers(0, 5, N_NODES, dtype=np.uint8),
        "row_ptr": np.arange(N_NODES + 1, dtype=np.uint32) * 15,
        "col_idx": rng.integers(0, N_NODES, N_EDGES, dtype=np.uint32),
        "weight": rng.random(N_EDGES) * 1e8,
    }


def percentiles(samples_ns):
    s = sorted(samples_ns)
    def at(q):
        idx = max(0, min(len(s) - 1, int(q * len(s)) - 1 if int(q * len(s)) > 0 else 0))
        return s[idx]
    return {
        "n": len(s),
        "mean_ns": statistics.fmean(s),
        "min_ns": s[0],
        "p50_ns": at(0.50),
        "p90_ns": at(0.90),
        "p99_ns": at(0.99),
        "p999_ns": at(0.999),
        "max_ns": s[-1],
    }


def bench_tobytes_file(payload, path):
    samples = []
    for _ in range(ITERS):
        t0 = time.perf_counter_ns()
        with open(path, "wb") as f:
            for arr in payload.values():
                f.write(arr.tobytes())
            f.write(np.array([N_NODES], dtype=np.uint32).tobytes())
            f.write(np.array([7500], dtype=np.uint32).tobytes())
            f.write(np.array([475], dtype=np.uint32).tobytes())
        samples.append(time.perf_counter_ns() - t0)
    return samples, os.path.getsize(path)


def bench_pickle(payload, path):
    samples = []
    for _ in range(ITERS):
        t0 = time.perf_counter_ns()
        with open(path, "wb") as f:
            pickle.dump(payload, f, protocol=pickle.HIGHEST_PROTOCOL)
        samples.append(time.perf_counter_ns() - t0)
    return samples, os.path.getsize(path)


def bench_pickle_roundtrip(payload, path):
    with open(path, "wb") as f:
        pickle.dump(payload, f, protocol=pickle.HIGHEST_PROTOCOL)
    samples = []
    for _ in range(ITERS):
        t0 = time.perf_counter_ns()
        with open(path, "rb") as f:
            obj = pickle.load(f)
        samples.append(time.perf_counter_ns() - t0)
        del obj
    return samples


def bench_shared_memory(payload):
    """What a real zero-copy handoff costs: one allocation, then numpy views
    written straight into the mapping. No serialization, no file."""
    total = sum(a.nbytes for a in payload.values())
    samples = []
    shm = shared_memory.SharedMemory(create=True, size=total)
    try:
        for _ in range(ITERS):
            t0 = time.perf_counter_ns()
            off = 0
            for arr in payload.values():
                view = np.ndarray(arr.shape, dtype=arr.dtype, buffer=shm.buf, offset=off)
                view[:] = arr
                off += arr.nbytes
            samples.append(time.perf_counter_ns() - t0)
    finally:
        shm.close()
        shm.unlink()
    return samples, total


def main():
    os.makedirs(OUT, exist_ok=True)
    payload = build_payload()
    tmp = os.path.join(OUT, "_bridge_tmp.bin")
    tmp_pkl = os.path.join(OUT, "_bridge_tmp.pkl")

    print("=" * 71)
    print("  bench_bridge — Python -> C++ handoff, three mechanisms")
    print(f"  python {sys.version.split()[0]}  numpy {np.__version__}")
    print(f"  iterations: {ITERS}")
    print("=" * 71)

    results = {}

    s, nbytes = bench_tobytes_file(payload, tmp)
    results["tobytes_file_write"] = percentiles(s) | {"bytes": nbytes}
    print(f"\n  1. tobytes + file write (AS IMPLEMENTED)   {nbytes:,} bytes")
    for k, v in results["tobytes_file_write"].items():
        print(f"       {k:10s} {v:,.0f}" if isinstance(v, float) else f"       {k:10s} {v:,}")

    s, nbytes = bench_pickle(payload, tmp_pkl)
    results["pickle_write"] = percentiles(s) | {"bytes": nbytes}
    print(f"\n  2. pickle write (BASELINE)                 {nbytes:,} bytes")
    for k, v in results["pickle_write"].items():
        print(f"       {k:10s} {v:,.0f}" if isinstance(v, float) else f"       {k:10s} {v:,}")

    s = bench_pickle_roundtrip(payload, tmp_pkl)
    results["pickle_read"] = percentiles(s)
    print("\n  3. pickle read (BASELINE)")
    for k, v in results["pickle_read"].items():
        print(f"       {k:10s} {v:,.0f}" if isinstance(v, float) else f"       {k:10s} {v:,}")

    s, nbytes = bench_shared_memory(payload)
    results["shared_memory_write"] = percentiles(s) | {"bytes": nbytes}
    print(f"\n  4. shared_memory write (WHAT ZERO-COPY WOULD COST — not implemented)  {nbytes:,} bytes")
    for k, v in results["shared_memory_write"].items():
        print(f"       {k:10s} {v:,.0f}" if isinstance(v, float) else f"       {k:10s} {v:,}")

    with open(os.path.join(OUT, "bridge_python.json"), "w") as f:
        json.dump(results, f, indent=2)
    print(f"\n  wrote {os.path.join(OUT, 'bridge_python.json')}")

    for p in (tmp, tmp_pkl):
        if os.path.exists(p):
            os.remove(p)


if __name__ == "__main__":
    main()
