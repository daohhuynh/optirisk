# OptiRisk — Measured Benchmarks

Generated 2026-09-28 08:02:04 UTC by `bench/run_all.sh` + `bench/make_report.py`.

Every number below is a measurement. Nothing here is estimated, extrapolated,
rounded up, or carried over from another machine. Where a measurement could not
be taken, the row says so rather than being omitted.


## 1. Hardware and toolchain

```
OptiRisk benchmark environment
generated: 2026-09-28T07:59:35Z
git commit: f9b8346
git dirty: no

── Hardware ──
cpu: INTEL(R) XEON(R) PLATINUM 8573C
arch: x86_64
logical cores: 4
os: Ubuntu 24.04.5 LTS
kernel: 6.17.0-1022-azure
avx2: yes
avx512f: yes
fma: yes
governor: performance
turbo (intel_pstate no_turbo): 0
isolcpus: 
THREAD PINNING: AVAILABLE (pthread_setaffinity_np)

── Toolchain ──
compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
target: x86_64-linux-gnu
flags: -std=c++23 -O3 -march=native -mavx2 -mfma -Wall -Wextra -Werror -Wno-interference-size -DNDEBUG -I/home/runner/work/optirisk/optirisk/backend/src -I/home/runner/work/optirisk/optirisk/bench
ldflags: -lpthread
python: Python 3.12.3
```

**Timer resolution:** `effective timer granularity: 6 ns (smallest nonzero back-to-back delta)`

This is the floor on every latency figure in this document. Any measurement
within a small multiple of it is quantization, not signal.


## 2. Methodology

**Clock.** Durations come from the project's own `read_timestamp()` (`RDTSC` on x86-64, `CNTVCT_EL0` on ARM64), converted through `ticks_to_ns()` whose rate is resolved at startup by `calibrate_timestamp_clock()`. No duration is derived from an assumed GHz — neither counter ticks at the core clock.

**Build.** Release only: `-O3 -march=native` plus `-mavx2 -mfma` on x86-64, matching `backend/CMakeLists.txt`, with `-Wall -Wextra -Werror`. The exact flags are recorded in the environment block above.

**Pinning.** Producer and consumer threads are pinned with `pthread_setaffinity_np` on Linux. Every result line records whether pinning actually succeeded (`pinned=1`/`pinned=0`). On macOS it cannot, and those runs are labelled not publication quality.

**Warmup.** Every benchmark discards a warmup prefix before sampling, so no figure includes cold-cache or cold-branch-predictor effects. Warmup counts are in each `bench/bench_*.cpp`.

**Allocation.** Sample storage is reserved before any timed region begins. No allocator is entered while the clock is running.

**Percentiles.** Nearest-rank on the sorted sample vector, no interpolation — every percentile reported is a sample that actually occurred. Raw per-sample CSVs are in `bench/results/raw_*.csv`.

**Pacing.** Latency and throughput are measured separately and never conflated. Latency runs pace the producer to hold the queue at depth ~1, so the number is handoff cost. Throughput runs saturate the producer; the latency observed there is queueing delay and is reported as such.

**State restoration.** `run_cascade_tick()` mutates the graph, the CLOB and the options book. Benchmarks restore all three between iterations, always outside the timed region, so every iteration performs the same work.

**Prefetch ablation.** Compares two builds of identical source — `OPTIRISK_NO_PREFETCH` compiles the eight `__builtin_prefetch` calls in `simd_engine.hpp` out. It is not a runtime flag, so no branch predictor can learn it.


## 3. Results

| Metric | Value | Baseline | Speedup | Conditions |
|---|---|---|---|---|
| Ring handoff latency, p50 | 36 ns | 2,342 ns (mutex+queue) | 65.06x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99 | 49 ns | 6,838 ns (mutex+queue) | 139.55x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99.9 | 186 ns | 10,874 ns (mutex+queue) | 58.46x | SPSC 1024-slot, paced to queue depth ~1 |
| Transport throughput | 51,993,000 events/s | 5,514,870 events/s (mutex+queue) | 9.43x | producer unpaced (saturated) |
| Full pipeline, p50 | 9,618 ns | — | — | 500 nodes/7500 edges; -30% equities on node 0; state restored between  |
| Full pipeline, p99 | 16,291 ns | — | — | same run |
| Cascade tick (real 500n/7500e), p50 | 7,175 ns | — | — | real 500 nodes/7500 edges; -30% equities on node 0; state restored per |
| Cascade tick (real 500n/7500e), p99 | 7,491 ns | — | — | same run |
| Prefetch ablation (cascade p50) | 7,175 ns (ON) | 7,163 ns (OFF) | 1.00x | same source, two builds |
| SIMD phases only, p50 | 1,705 ns | 1,684 ns (prefetch OFF) | 0.99x | apply_shock_simd, 500 nodes |
| Option kernel throughput | 1,061.55 M options/s | 69.05 M options/s (std::erfc f64) | 15.37x | single core, batch=496, kernel path=AVX2-8lane-f32 |
| Delta max abs error | 8.003e-07 | double-precision reference | — | full grid, finite results only |
| Delta mean abs error | 1.415e-07 | double-precision reference | — | full grid |
| Fast-log max abs error | 1.176e-07 | double-precision reference | — | S/K in [0.70, 1.50] |
| rcp+NR max rel error | 1.618e-07 | double-precision reference | — | p in [1, 2000] |
| CLOB fill latency, p50 | 50 ns | — | — | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fi |
| CLOB fill latency, p99 | 110 ns | — | — | same run |
| CLOB fill throughput | 1,609,270 fills/s | — | — | includes refresh_liquidity re-quote per fill; pinned=1 |
| BBO publish -> observed, p50 | 45 ns | — | — | compute stamps then flip_buffers(); reader spins on active_buffer_idx  |
| BBO publish -> observed, p99 | 57 ns | — | — | same run |
| Python->C++ graph load (C++ side) | 12,811 ns | 2,895 ns (in-process memcpy floor) | 4.43x SLOWER | fopen + 21 fread, warm page cache |

### 3.1 `O(levels consumed)` — cost vs levels consumed

| Levels consumed | p50 (ns) | p99 (ns) | ns per level |
|---|---|---|---|
| 1 | 13 | 19 | 13.00 |
| 2 | 14 | 19 | 7.00 |
| 4 | 16 | 20 | 4.00 |
| 8 | 19 | 25 | 2.38 |
| 16 | 26 | 32 | 1.62 |
| 32 | 57 | 64 | 1.78 |
| 64 | 123 | 130 | 1.92 |
| 128 | 256 | 265 | 2.00 |
| 256 | 549 | 560 | 2.14 |

### 3.2 `O(levels consumed)` — cost vs book depth (1 level consumed)

Flat here is the claim. Growth would mean the book shifts memory on consumption.

| Book depth | p50 (ns) | p99 (ns) |
|---|---|---|
| 8 | 14 | 19 |
| 16 | 14 | 19 |
| 32 | 14 | 19 |
| 64 | 14 | 19 |
| 128 | 14 | 19 |
| 256 | 14 | 19 |

### 3.3 Cascade cost by graph size

| Graph | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|
| N=100 E=1504 | 592,628 | 629,681 | 660,000 | 719,494 |
| N=200 E=3004 | 1,112,023 | 1,159,733 | 1,444,536 | 1,642,444 |
| N=300 E=4483 | 1,593,294 | 1,636,703 | 1,741,708 | 1,942,047 |
| N=400 E=5994 | 2,071,220 | 2,575,795 | 3,362,226 | 4,979,432 |
| N=500 E=7475 | 2,554,212 | 2,622,961 | 2,748,836 | 2,897,371 |
| N=100 E=1504 | 544,767 | 574,842 | 710,490 | 897,546 |
| N=200 E=3004 | 1,007,950 | 1,048,285 | 1,169,214 | 1,339,050 |
| N=300 E=4483 | 1,446,733 | 1,495,219 | 1,630,117 | 1,736,171 |
| N=400 E=5994 | 1,888,878 | 1,965,205 | 2,189,841 | 2,680,527 |
| N=500 E=7475 | 2,339,131 | 2,406,878 | 2,598,145 | 2,937,372 |

### 3.4 Branchless claim — disassembly

`bench/results/blackscholes_disasm.txt`: **4 conditional branches in probe_black_scholes**

A nonzero count means the hot loop is not branchless even where the arithmetic inside it is.


### 3.5 Python-side handoff

| Mechanism | p50 (ns) | p99 (ns) | bytes |
|---|---|---|---|
| tobytes_file_write | 590,025 | 1,055,670 | 143820 |
| pickle_write | 583,429 | 1,016,311 | 144737 |
| pickle_read | 32,031 | 52,602 | — |
| shared_memory_write | 16,838 | 26,730 | 143808 |

## 4. Full percentile output

Complete per-metric percentiles, exactly as emitted:

| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cascade | N=100 E=1504 | AVX2/prefetch=ON | 5000 | 593440.2 | 575,372 | 592,628 | 598,009 | 629,681 | 660,000 | 719,494 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=ON | 5000 | 1114563.2 | 1,085,616 | 1,112,023 | 1,129,494 | 1,159,733 | 1,444,536 | 1,642,444 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=ON | 5000 | 1594850.8 | 1,559,753 | 1,593,294 | 1,614,394 | 1,636,703 | 1,741,708 | 1,942,047 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=ON | 5000 | 2086770.7 | 2,025,545 | 2,071,220 | 2,103,244 | 2,575,795 | 3,362,226 | 4,979,432 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=ON | 5000 | 2556843.2 | 2,502,630 | 2,554,212 | 2,581,723 | 2,622,961 | 2,748,836 | 2,897,371 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=ON | 5000 | 7218.9 | 6,900 | 7,175 | 7,223 | 7,491 | 12,158 | 14,419 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=ON | 5000 | 1867.0 | 1,660 | 1,705 | 2,543 | 2,759 | 8,927 | 29,481 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=ON | 500 | 12932.6 | 12,694 | 12,811 | 12,874 | 18,212 | 20,943 | 20,943 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=ON | 500 | 3035.2 | 2,799 | 2,895 | 2,930 | 3,284 | 53,636 | 53,636 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| cascade | N=100 E=1504 | AVX2/prefetch=OFF | 5000 | 545553.2 | 533,620 | 544,767 | 549,458 | 574,842 | 710,490 | 897,546 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=OFF | 5000 | 1009495.9 | 985,043 | 1,007,950 | 1,018,673 | 1,048,285 | 1,169,214 | 1,339,050 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=OFF | 5000 | 1447328.6 | 1,416,406 | 1,446,733 | 1,463,203 | 1,495,219 | 1,630,117 | 1,736,171 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=OFF | 5000 | 1892038.2 | 1,858,472 | 1,888,878 | 1,909,772 | 1,965,205 | 2,189,841 | 2,680,527 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=OFF | 5000 | 2342531.0 | 2,303,389 | 2,339,131 | 2,361,943 | 2,406,878 | 2,598,145 | 2,937,372 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=OFF | 5000 | 7226.2 | 6,922 | 7,163 | 7,378 | 7,488 | 12,058 | 18,453 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=OFF | 5000 | 1746.5 | 1,643 | 1,684 | 1,723 | 2,279 | 11,409 | 27,460 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=OFF | 500 | 12882.7 | 12,651 | 12,768 | 12,861 | 17,618 | 27,778 | 27,778 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=OFF | 500 | 3043.0 | 2,794 | 2,892 | 2,924 | 3,725 | 52,899 | 52,899 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| clob | fill_levels_1 | AVX2/prefetch=ON | 20000 | 14.2 | 9 | 13 | 18 | 19 | 19 | 53 | depth=256, levels consumed=1 (observed 1), no BBO recording; pinned=1 |
| clob | fill_levels_2 | AVX2/prefetch=ON | 20000 | 14.9 | 10 | 14 | 18 | 19 | 19 | 60 | depth=256, levels consumed=2 (observed 2), no BBO recording; pinned=1 |
| clob | fill_levels_4 | AVX2/prefetch=ON | 20000 | 16.3 | 11 | 16 | 19 | 20 | 21 | 120 | depth=256, levels consumed=4 (observed 4), no BBO recording; pinned=1 |
| clob | fill_levels_8 | AVX2/prefetch=ON | 20000 | 20.5 | 14 | 19 | 23 | 25 | 33 | 5,244 | depth=256, levels consumed=8 (observed 8), no BBO recording; pinned=1 |
| clob | fill_levels_16 | AVX2/prefetch=ON | 20000 | 27.1 | 21 | 26 | 31 | 32 | 33 | 70 | depth=256, levels consumed=16 (observed 16), no BBO recording; pinned=1 |
| clob | fill_levels_32 | AVX2/prefetch=ON | 20000 | 57.9 | 49 | 57 | 61 | 64 | 73 | 9,636 | depth=256, levels consumed=32 (observed 32), no BBO recording; pinned=1 |
| clob | fill_levels_64 | AVX2/prefetch=ON | 20000 | 123.8 | 114 | 123 | 127 | 130 | 132 | 4,878 | depth=256, levels consumed=64 (observed 64), no BBO recording; pinned=1 |
| clob | fill_levels_128 | AVX2/prefetch=ON | 20000 | 257.6 | 246 | 256 | 260 | 265 | 300 | 9,963 | depth=256, levels consumed=128 (observed 128), no BBO recording; pinned=1 |
| clob | fill_levels_256 | AVX2/prefetch=ON | 20000 | 554.0 | 536 | 549 | 555 | 560 | 565 | 9,436 | depth=256, levels consumed=256 (observed 256), no BBO recording; pinned=1 |
| clob | fill_depth_8 | AVX2/prefetch=ON | 20000 | 14.9 | 9 | 14 | 18 | 19 | 19 | 40 | book depth=8, 1 level consumed; pinned=1 |
| clob | fill_depth_16 | AVX2/prefetch=ON | 20000 | 15.4 | 9 | 14 | 18 | 19 | 19 | 8,114 | book depth=16, 1 level consumed; pinned=1 |
| clob | fill_depth_32 | AVX2/prefetch=ON | 20000 | 15.0 | 9 | 14 | 18 | 19 | 19 | 41 | book depth=32, 1 level consumed; pinned=1 |
| clob | fill_depth_64 | AVX2/prefetch=ON | 20000 | 14.8 | 9 | 14 | 18 | 19 | 19 | 33 | book depth=64, 1 level consumed; pinned=1 |
| clob | fill_depth_128 | AVX2/prefetch=ON | 20000 | 15.3 | 9 | 14 | 18 | 19 | 19 | 8,128 | book depth=128, 1 level consumed; pinned=1 |
| clob | fill_depth_256 | AVX2/prefetch=ON | 20000 | 14.9 | 9 | 14 | 18 | 19 | 19 | 25 | book depth=256, 1 level consumed; pinned=1 |
| clob | fill_latency_mixed | AVX2/prefetch=ON | 200000 | 52.3 | 12 | 50 | 87 | 110 | 125 | 8,173 | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=1 |
| clob | bbo_publish_latency | AVX2/prefetch=ON | 100000 | 45.2 | 22 | 45 | 53 | 57 | 64 | 8,399 | compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=1 |
| noise | fixed_work_unit | AVX2/prefetch=ON | 2000000 | 193.8 | 179 | 193 | 199 | 203 | 261 | 47,838 | identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=ON | 2500000 | 66.1 | 20 | 36 | 46 | 49 | 186 | 312,370 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=ON | 2500000 | 2671.3 | 63 | 2,342 | 4,826 | 6,838 | 10,874 | 324,753 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_end_to_end | AVX2/prefetch=ON | 20000 | 10166.8 | 8,572 | 9,618 | 11,121 | 16,291 | 20,047 | 29,702 | 500 nodes/7500 edges; -30% equities on node 0; state restored between events (untimed); paced 300us; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=OFF | 2500000 | 58.6 | 20 | 36 | 46 | 51 | 959 | 313,101 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=OFF | 2500000 | 2640.3 | 63 | 2,321 | 4,693 | 6,775 | 10,887 | 309,326 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_end_to_end | AVX2/prefetch=OFF | 20000 | 9613.2 | 8,441 | 8,986 | 10,664 | 13,670 | 18,132 | 38,538 | 500 nodes/7500 edges; -30% equities on node 0; state restored between events (untimed); paced 300us; pinned=1 |

### Scalar metrics

| Bench | Metric | Build | Value | Unit | Conditions |
|---|---|---|---|---|---|
| blackscholes | delta_nonfinite_count | AVX2/prefetch=ON | 126 | count | inputs where the kernel returned NaN/Inf instead of a delta |
| blackscholes | delta_max_abs_err | AVX2/prefetch=ON | 8.00252e-07 | abs | full grid, finite results only |
| blackscholes | delta_mean_abs_err | AVX2/prefetch=ON | 1.41538e-07 | abs | full grid |
| blackscholes | delta_max_abs_err_atm | AVX2/prefetch=ON | 7.07965e-07 | abs | |ln(S/K)|<=0.05 |
| blackscholes | delta_max_abs_err_far | AVX2/prefetch=ON | 8.00252e-07 | abs | |ln(S/K)|>0.20 |
| blackscholes | fastlog_max_abs_err | AVX2/prefetch=ON | 1.17632e-07 | abs | S/K in [0.70, 1.50] |
| blackscholes | rcp_nr_max_rel_err | AVX2/prefetch=ON | 1.61759e-07 | relative | p in [1, 2000] |
| blackscholes | kernel_throughput_ops | AVX2/prefetch=ON | 1.06155e+09 | options/s | single core, batch=496, kernel path=AVX2-8lane-f32 |
| blackscholes | scalar_throughput_ops | AVX2/prefetch=ON | 6.90542e+07 | options/s | std::erfc f64, single core, batch=496 |
| blackscholes | speedup_vs_scalar | AVX2/prefetch=ON | 15.3727 | x | same batch |
| bridge | graph_bytes | AVX2/prefetch=ON | 144448 | bytes | sizeof(CSRGraph) |
| bridge | graph_bytes | AVX2/prefetch=OFF | 144448 | bytes | sizeof(CSRGraph) |
| clob | fill_throughput_fps | AVX2/prefetch=ON | 1.60927e+06 | fills/s | includes refresh_liquidity re-quote per fill; pinned=1 |
| noise | p99_over_p50 | AVX2/prefetch=ON | 1.05181 | ratio | 1.00 = interference does not reach p99 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=ON | 0 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=ON | 4.74501 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=ON | 5.1993e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=ON | 5.51487e+06 | events/s | unpaced; mutex+queue; pinned=1 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=OFF | 10.2041 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=OFF | 2.90265 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=OFF | 5.20453e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=OFF | 5.48511e+06 | events/s | unpaced; mutex+queue; pinned=1 |

## 5. Reproducing

```sh
./bench/run_all.sh          # build both variants, run everything, regenerate this file
./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness
```

Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, `bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.

