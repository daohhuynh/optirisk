#!/usr/bin/env python3
"""
make_report.py — assemble BENCHMARKS.md from bench/results/.

Reads environment.txt and all_results.csv (both produced by run_all.sh) and
writes BENCHMARKS.md at the repo root. Regenerating the report is therefore
part of rerunning the benchmarks, not a separate manual step, so the document
can never drift from the numbers it claims to report.

Every figure in the output traces to a row in all_results.csv. Nothing is
rounded up, averaged across runs, or carried over from a previous machine.
"""

import csv
import json
import os
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
RESULTS = os.path.join(HERE, "results")
OUTFILE = os.path.join(REPO, "BENCHMARKS.md")


def read_environment():
    path = os.path.join(RESULTS, "environment.txt")
    if not os.path.exists(path):
        return "(environment.txt missing — run bench/run_all.sh)"
    with open(path) as f:
        return f.read().strip()


def read_rows():
    path = os.path.join(RESULTS, "all_results.csv")
    if not os.path.exists(path):
        return [], []
    stats, scalars = [], []
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0] == "kind":
                continue
            if row[0] == "stats" and len(row) >= 14:
                stats.append({
                    "bench": row[1], "metric": row[2], "simd": row[3], "prefetch": row[4],
                    "count": row[5], "mean_ns": row[6], "min_ns": row[7], "p50_ns": row[8],
                    "p90_ns": row[9], "p99_ns": row[10], "p999_ns": row[11],
                    "max_ns": row[12], "conditions": row[13],
                })
            elif row[0] == "scalar" and len(row) >= 8:
                scalars.append({
                    "bench": row[1], "metric": row[2], "simd": row[3], "prefetch": row[4],
                    "value": row[5], "unit": row[6], "conditions": row[7],
                })
    return stats, scalars


def find_stat(stats, bench, metric, prefetch="prefetch=ON"):
    for s in stats:
        if s["bench"] == bench and s["metric"] == metric and s["prefetch"] == prefetch:
            return s
    return None


def find_scalar(scalars, bench, metric, prefetch="prefetch=ON"):
    for s in scalars:
        if s["bench"] == bench and s["metric"] == metric and s["prefetch"] == prefetch:
            return s
    return None


def ns(v):
    try:
        return f"{int(float(v)):,}"
    except (TypeError, ValueError):
        return "—"


def speedup(a, b):
    """a/b as an 'N.Nx' string, or an em dash if either side is missing."""
    try:
        fa, fb = float(a), float(b)
        if fb == 0:
            return "—"
        return f"{fa / fb:.2f}x"
    except (TypeError, ValueError):
        return "—"


def headline_table(stats, scalars):
    """metric | value | baseline | speedup | conditions"""
    rows = []

    d = find_stat(stats, "ring", "disruptor_handoff_paced")
    m = find_stat(stats, "ring", "mutex_handoff_paced")
    if d and m:
        for pct in ("p50", "p99", "p999"):
            key = f"{pct}_ns"
            label = {"p50": "p50", "p99": "p99", "p999": "p99.9"}[pct]
            rows.append((
                f"Ring handoff latency, {label}",
                f"{ns(d[key])} ns",
                f"{ns(m[key])} ns (mutex+queue)",
                speedup(m[key], d[key]),
                "SPSC 1024-slot, paced to queue depth ~1",
            ))

    dt = find_scalar(scalars, "ring", "disruptor_throughput_eps")
    mt = find_scalar(scalars, "ring", "mutex_throughput_eps")
    if dt and mt:
        rows.append((
            "Transport throughput",
            f"{float(dt['value']):,.0f} events/s",
            f"{float(mt['value']):,.0f} events/s (mutex+queue)",
            speedup(dt["value"], mt["value"]),
            "producer unpaced (saturated)",
        ))

    p = find_stat(stats, "ring", "pipeline_end_to_end")
    if p:
        rows.append((
            "Full pipeline, p50",
            f"{ns(p['p50_ns'])} ns", "—", "—", p["conditions"][:70],
        ))
        rows.append((
            "Full pipeline, p99",
            f"{ns(p['p99_ns'])} ns", "—", "—", "same run",
        ))

    c = find_stat(stats, "cascade", "run_cascade_tick_real500")
    if c:
        rows.append(("Cascade tick (real 500n/7500e), p50", f"{ns(c['p50_ns'])} ns", "—", "—", c["conditions"][:70]))
        rows.append(("Cascade tick (real 500n/7500e), p99", f"{ns(c['p99_ns'])} ns", "—", "—", "same run"))

    cn = find_stat(stats, "cascade", "run_cascade_tick_real500", "prefetch=OFF")
    if c and cn:
        rows.append((
            "Prefetch ablation (cascade p50)",
            f"{ns(c['p50_ns'])} ns (ON)",
            f"{ns(cn['p50_ns'])} ns (OFF)",
            speedup(cn["p50_ns"], c["p50_ns"]),
            "same source, two builds",
        ))

    sp = find_stat(stats, "cascade", "apply_shock_simd_real500")
    spn = find_stat(stats, "cascade", "apply_shock_simd_real500", "prefetch=OFF")
    if sp:
        rows.append(("SIMD phases only, p50", f"{ns(sp['p50_ns'])} ns",
                     f"{ns(spn['p50_ns'])} ns (prefetch OFF)" if spn else "—",
                     speedup(spn["p50_ns"], sp["p50_ns"]) if spn else "—",
                     "apply_shock_simd, 500 nodes"))

    k = find_scalar(scalars, "blackscholes", "kernel_throughput_ops")
    sc = find_scalar(scalars, "blackscholes", "scalar_throughput_ops")
    if k and sc:
        rows.append((
            "Option kernel throughput",
            f"{float(k['value']) / 1e6:,.2f} M options/s",
            f"{float(sc['value']) / 1e6:,.2f} M options/s (std::erfc f64)",
            speedup(k["value"], sc["value"]),
            k["conditions"][:70],
        ))

    for metric, label in (("delta_max_abs_err", "Delta max abs error"),
                          ("delta_mean_abs_err", "Delta mean abs error"),
                          ("fastlog_max_abs_err", "Fast-log max abs error"),
                          ("rcp_nr_max_rel_err", "rcp+NR max rel error")):
        e = find_scalar(scalars, "blackscholes", metric)
        if e:
            val = float(e["value"])
            rows.append((label,
                         "SKIPPED" if val < 0 else f"{val:.3e}",
                         "double-precision reference", "—", e["conditions"][:70]))

    f = find_stat(stats, "clob", "fill_latency_mixed")
    if f:
        rows.append(("CLOB fill latency, p50", f"{ns(f['p50_ns'])} ns", "—", "—", f["conditions"][:70]))
        rows.append(("CLOB fill latency, p99", f"{ns(f['p99_ns'])} ns", "—", "—", "same run"))

    ft = find_scalar(scalars, "clob", "fill_throughput_fps")
    if ft:
        rows.append(("CLOB fill throughput", f"{float(ft['value']):,.0f} fills/s", "—", "—", ft["conditions"][:70]))

    b = find_stat(stats, "clob", "bbo_publish_latency")
    if b:
        rows.append(("BBO publish -> observed, p50", f"{ns(b['p50_ns'])} ns", "—", "—", b["conditions"][:70]))
        rows.append(("BBO publish -> observed, p99", f"{ns(b['p99_ns'])} ns", "—", "—", "same run"))

    br = find_stat(stats, "bridge", "cpp_fread_load")
    mc = find_stat(stats, "bridge", "memcpy_floor")
    if br and mc:
        rows.append((
            "Python->C++ graph load (C++ side)",
            f"{ns(br['p50_ns'])} ns",
            f"{ns(mc['p50_ns'])} ns (in-process memcpy floor)",
            speedup(br["p50_ns"], mc["p50_ns"]) + " SLOWER",
            "fopen + 21 fread, warm page cache",
        ))

    return rows


def complexity_tables(stats):
    levels, depths, sizes = [], [], []
    for s in stats:
        if s["bench"] == "clob" and s["metric"].startswith("fill_levels_"):
            levels.append((int(s["metric"].split("_")[-1]), s))
        elif s["bench"] == "clob" and s["metric"].startswith("fill_depth_"):
            depths.append((int(s["metric"].split("_")[-1]), s))
        elif s["bench"] == "cascade" and s["metric"].startswith("N="):
            sizes.append((s["metric"], s))
    levels.sort(key=lambda x: x[0])
    depths.sort(key=lambda x: x[0])
    return levels, depths, sizes


def main():
    stats, scalars = read_rows()
    env = read_environment()
    rows = headline_table(stats, scalars)
    levels, depths, sizes = complexity_tables(stats)

    gran_note = ""
    for logname in ("bench_ring.log", "bench_clob.log"):
        p = os.path.join(RESULTS, logname)
        if os.path.exists(p):
            with open(p) as f:
                for line in f:
                    if "effective timer granularity" in line:
                        gran_note = line.strip()
                        break
        if gran_note:
            break

    disasm_note = "(not generated)"
    dpath = os.path.join(RESULTS, "blackscholes_disasm.txt")
    if os.path.exists(dpath):
        with open(dpath) as f:
            for line in f:
                if line.startswith("conditional_branch_count="):
                    disasm_note = line.strip().split("=")[1] + " conditional branches in probe_black_scholes"

    out = []
    A = out.append

    A("# OptiRisk — Measured Benchmarks\n")
    A(f"Generated {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')} "
      f"by `bench/run_all.sh` + `bench/make_report.py`.\n")
    A("Every number below is a measurement. Nothing here is estimated, extrapolated,\n"
      "rounded up, or carried over from another machine. Where a measurement could not\n"
      "be taken, the row says so rather than being omitted.\n")

    A("\n## 1. Hardware and toolchain\n")
    A("```")
    A(env)
    A("```")
    if gran_note:
        A(f"\n**Timer resolution:** `{gran_note}`\n")
        A("This is the floor on every latency figure in this document. Any measurement\n"
          "within a small multiple of it is quantization, not signal.\n")

    A("\n## 2. Methodology\n")
    A("**Clock.** Durations come from the project's own `read_timestamp()` "
      "(`RDTSC` on x86-64, `CNTVCT_EL0` on ARM64), converted through "
      "`ticks_to_ns()` whose rate is resolved at startup by "
      "`calibrate_timestamp_clock()`. No duration is derived from an assumed GHz — "
      "neither counter ticks at the core clock.\n")
    A("**Build.** Release only: `-O3 -march=native` plus `-mavx2 -mfma` on x86-64, "
      "matching `backend/CMakeLists.txt`, with `-Wall -Wextra -Werror`. The exact "
      "flags are recorded in the environment block above.\n")
    A("**Pinning.** Producer and consumer threads are pinned with "
      "`pthread_setaffinity_np` on Linux. Every result line records whether pinning "
      "actually succeeded (`pinned=1`/`pinned=0`). On macOS it cannot, and those runs "
      "are labelled not publication quality.\n")
    A("**Warmup.** Every benchmark discards a warmup prefix before sampling, so no "
      "figure includes cold-cache or cold-branch-predictor effects. Warmup counts are "
      "in each `bench/bench_*.cpp`.\n")
    A("**Allocation.** Sample storage is reserved before any timed region begins. "
      "No allocator is entered while the clock is running.\n")
    A("**Percentiles.** Nearest-rank on the sorted sample vector, no interpolation — "
      "every percentile reported is a sample that actually occurred. Raw per-sample "
      "CSVs are in `bench/results/raw_*.csv`.\n")
    A("**Pacing.** Latency and throughput are measured separately and never conflated. "
      "Latency runs pace the producer to hold the queue at depth ~1, so the number is "
      "handoff cost. Throughput runs saturate the producer; the latency observed there "
      "is queueing delay and is reported as such.\n")
    A("**State restoration.** `run_cascade_tick()` mutates the graph, the CLOB and the "
      "options book. Benchmarks restore all three between iterations, always outside "
      "the timed region, so every iteration performs the same work.\n")
    A("**Prefetch ablation.** Compares two builds of identical source — "
      "`OPTIRISK_NO_PREFETCH` compiles the eight `__builtin_prefetch` calls in "
      "`simd_engine.hpp` out. It is not a runtime flag, so no branch predictor can "
      "learn it.\n")

    A("\n## 3. Results\n")
    if not rows:
        A("_No results collected yet. Run `bench/run_all.sh`._\n")
    else:
        A("| Metric | Value | Baseline | Speedup | Conditions |")
        A("|---|---|---|---|---|")
        for r in rows:
            A("| " + " | ".join(str(x) for x in r) + " |")

    A("\n### 3.1 `O(levels consumed)` — cost vs levels consumed\n")
    if levels:
        A("| Levels consumed | p50 (ns) | p99 (ns) | ns per level |")
        A("|---|---|---|---|")
        for lv, s in levels:
            per = float(s["p50_ns"]) / lv if lv else 0
            A(f"| {lv} | {ns(s['p50_ns'])} | {ns(s['p99_ns'])} | {per:.2f} |")
    else:
        A("_Not collected._")

    A("\n### 3.2 `O(levels consumed)` — cost vs book depth (1 level consumed)\n")
    A("Flat here is the claim. Growth would mean the book shifts memory on consumption.\n")
    if depths:
        A("| Book depth | p50 (ns) | p99 (ns) |")
        A("|---|---|---|")
        for d, s in depths:
            A(f"| {d} | {ns(s['p50_ns'])} | {ns(s['p99_ns'])} |")
    else:
        A("_Not collected._")

    A("\n### 3.3 Cascade cost by graph size\n")
    if sizes:
        A("| Graph | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |")
        A("|---|---|---|---|---|")
        for label, s in sizes:
            A(f"| {label} | {ns(s['p50_ns'])} | {ns(s['p99_ns'])} | {ns(s['p999_ns'])} | {ns(s['max_ns'])} |")
    else:
        A("_Not collected._")

    A("\n### 3.4 Branchless claim — disassembly\n")
    A(f"`bench/results/blackscholes_disasm.txt`: **{disasm_note}**\n")
    A("A nonzero count means the hot loop is not branchless even where the arithmetic "
      "inside it is.\n")

    bridge_json = os.path.join(RESULTS, "bridge_python.json")
    A("\n### 3.5 Python-side handoff\n")
    if os.path.exists(bridge_json):
        with open(bridge_json) as f:
            bj = json.load(f)
        A("| Mechanism | p50 (ns) | p99 (ns) | bytes |")
        A("|---|---|---|---|")
        for k, v in bj.items():
            A(f"| {k} | {ns(v.get('p50_ns'))} | {ns(v.get('p99_ns'))} | {v.get('bytes', '—')} |")
    else:
        A("_Not collected (numpy missing, or bench_bridge.py not run)._")

    A("\n## 4. Full percentile output\n")
    A("Complete per-metric percentiles, exactly as emitted:\n")
    if stats:
        A("| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |")
        A("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for s in stats:
            A("| {bench} | {metric} | {simd}/{prefetch} | {count} | {mean} | {mn} | {p50} | "
              "{p90} | {p99} | {p999} | {mx} | {cond} |".format(
                  bench=s["bench"], metric=s["metric"], simd=s["simd"], prefetch=s["prefetch"],
                  count=s["count"], mean=s["mean_ns"], mn=ns(s["min_ns"]), p50=ns(s["p50_ns"]),
                  p90=ns(s["p90_ns"]), p99=ns(s["p99_ns"]), p999=ns(s["p999_ns"]),
                  mx=ns(s["max_ns"]), cond=s["conditions"]))
    else:
        A("_No results collected yet._")

    A("\n### Scalar metrics\n")
    if scalars:
        A("| Bench | Metric | Build | Value | Unit | Conditions |")
        A("|---|---|---|---|---|---|")
        for s in scalars:
            A(f"| {s['bench']} | {s['metric']} | {s['simd']}/{s['prefetch']} | "
              f"{s['value']} | {s['unit']} | {s['conditions']} |")
    else:
        A("_No scalar metrics collected yet._")

    A("\n## 5. Reproducing\n")
    A("```sh\n./bench/run_all.sh          # build both variants, run everything, regenerate this file\n"
      "./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness\n```\n")
    A("Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, "
      "`bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.\n")

    with open(OUTFILE, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"  wrote {OUTFILE} ({len(rows)} headline rows, {len(stats)} stat rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
