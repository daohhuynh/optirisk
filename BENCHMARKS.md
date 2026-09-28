# OptiRisk — Measured Benchmarks

Generated 2026-09-28 08:26:25 UTC by `bench/run_all.sh` + `bench/make_report.py`.

Every number below is a measurement. Nothing here is estimated, extrapolated,
rounded up, or carried over from another machine. Where a measurement could not
be taken, the row says so rather than being omitted.


## 1. Hardware and toolchain

```
OptiRisk benchmark environment
generated: 2026-09-28T08:23:40Z
git commit: 1ef5b52
git dirty: no

── Hardware ──
cpu: AMD EPYC 7763 64-Core Processor
arch: x86_64
logical cores: 4
os: Ubuntu 24.04.5 LTS
kernel: 6.17.0-1022-azure
avx2: yes
avx512f: no
fma: yes
governor: unknown
turbo (intel_pstate no_turbo): unknown
isolcpus: 
THREAD PINNING: AVAILABLE (pthread_setaffinity_np)

── Toolchain ──
compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
target: x86_64-linux-gnu
flags: -std=c++23 -O3 -march=native -mavx2 -mfma -Wall -Wextra -Werror -Wno-interference-size -DNDEBUG -I/home/runner/work/optirisk/optirisk/backend/src -I/home/runner/work/optirisk/optirisk/bench
ldflags: -lpthread
python: Python 3.12.3
```

**Timer resolution:** `effective timer granularity: 9 ns (smallest nonzero back-to-back delta)`

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
| Ring handoff latency, p50 | 27 ns | 5,218 ns (mutex+queue) | 193.26x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99 | 1,180 ns | 18,262 ns (mutex+queue) | 15.48x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99.9 | 7,543,548,267,621,015,552 ns | 20,496 ns (mutex+queue) | 0.00x | SPSC 1024-slot, paced to queue depth ~1 |
| Transport throughput | 46,928,700 events/s | 4,621,390 events/s (mutex+queue) | 10.15x | producer unpaced (saturated) |
| Full pipeline, p50 | 11,058 ns | — | — | 500 nodes/7500 edges; -30% equities on node 0; state restored between  |
| Full pipeline, p99 | 23,632 ns | — | — | same run |
| Cascade tick (real 500n/7500e), p50 | 7,023 ns | — | — | real 500 nodes/7500 edges; -30% equities on node 0; state restored per |
| Cascade tick (real 500n/7500e), p99 | 7,273 ns | — | — | same run |
| Prefetch ablation (cascade p50) | 7,023 ns (ON) | 7,113 ns (OFF) | 1.01x | same source, two builds |
| SIMD phases only, p50 | 1,793 ns | 2,144 ns (prefetch OFF) | 1.20x | apply_shock_simd, 500 nodes |
| Option kernel throughput | 639.62 M options/s | 54.68 M options/s (std::erfc f64) | 11.70x | single core, batch=496, kernel path=AVX2-8lane-f32 |
| Delta max abs error | 7.406e-07 | double-precision reference | — | full grid, finite results only |
| Delta mean abs error | 1.394e-07 | double-precision reference | — | full grid |
| Fast-log max abs error | 1.176e-07 | double-precision reference | — | S/K in [0.70, 1.50] |
| rcp+NR max rel error | 1.645e-07 | double-precision reference | — | p in [1, 2000] |
| CLOB fill latency, p50 | 40 ns | — | — | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fi |
| CLOB fill latency, p99 | 70 ns | — | — | same run |
| CLOB fill throughput | 2,503,810 fills/s | — | — | includes refresh_liquidity re-quote per fill; pinned=1 |
| BBO publish -> observed, p50 | 28 ns | — | — | compute stamps then flip_buffers(); reader spins on active_buffer_idx  |
| BBO publish -> observed, p99 | 68 ns | — | — | same run |
| Python->C++ graph load (C++ side) | 23,694 ns | 2,975 ns (in-process memcpy floor) | 7.96x SLOWER | fopen + 21 fread, warm page cache |

### 3.1 `O(levels consumed)` — cost vs levels consumed

| Levels consumed | p50 (ns) | p99 (ns) | ns per level |
|---|---|---|---|
| 1 | 10 | 20 | 10.00 |
| 2 | 10 | 20 | 5.00 |
| 4 | 10 | 20 | 2.50 |
| 8 | 10 | 20 | 1.25 |
| 16 | 20 | 40 | 1.25 |
| 32 | 49 | 60 | 1.53 |
| 64 | 130 | 140 | 2.03 |
| 128 | 230 | 250 | 1.80 |
| 256 | 410 | 471 | 1.60 |

### 3.2 `O(levels consumed)` — cost vs book depth (1 level consumed)

Flat here is the claim. Growth would mean the book shifts memory on consumption.

| Book depth | p50 (ns) | p99 (ns) |
|---|---|---|
| 8 | 10 | 20 |
| 16 | 10 | 20 |
| 32 | 10 | 20 |
| 64 | 10 | 20 |
| 128 | 10 | 20 |
| 256 | 10 | 20 |

### 3.3 Cascade cost by graph size

| Graph | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|
| N=100 E=1504 | 678,501 | 706,254 | 774,383 | 914,347 |
| N=200 E=3004 | 1,229,413 | 1,300,687 | 1,588,020 | 2,073,166 |
| N=300 E=4483 | 1,849,985 | 1,908,746 | 2,289,405 | 3,512,175 |
| N=400 E=5994 | 2,410,363 | 2,545,198 | 4,029,172 | 5,228,157 |
| N=500 E=7475 | 2,970,923 | 3,062,506 | 3,286,469 | 4,709,457 |
| N=100 E=1504 | 598,891 | 774,863 | 1,101,610 | 1,451,732 |
| N=200 E=3004 | 1,066,114 | 1,136,096 | 1,287,392 | 1,940,986 |
| N=300 E=4483 | 1,579,203 | 1,666,348 | 2,096,430 | 2,905,900 |
| N=400 E=5994 | 2,077,735 | 2,165,631 | 2,312,448 | 3,321,064 |
| N=500 E=7475 | 2,618,296 | 2,728,464 | 2,818,915 | 3,011,028 |

### 3.4 Branchless claim — disassembly

`bench/results/blackscholes_disasm.txt`: **4 conditional branches in probe_black_scholes**

A nonzero count means the hot loop is not branchless even where the arithmetic inside it is.


### 3.5 Python-side handoff

| Mechanism | p50 (ns) | p99 (ns) | bytes |
|---|---|---|---|
| tobytes_file_write | 380,292 | 535,653 | 143820 |
| pickle_write | 384,370 | 566,080 | 144737 |
| pickle_read | 66,284 | 95,770 | — |
| shared_memory_write | 28,343 | 51,076 | 143808 |

## 4. Full percentile output

Complete per-metric percentiles, exactly as emitted:

| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cascade | N=100 E=1504 | AVX2/prefetch=ON | 5000 | 677802.7 | 667,992 | 678,501 | 681,918 | 706,254 | 774,383 | 914,347 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=ON | 5000 | 1234044.8 | 1,224,413 | 1,229,413 | 1,240,543 | 1,300,687 | 1,588,020 | 2,073,166 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=ON | 5000 | 1854530.6 | 1,835,507 | 1,849,985 | 1,863,520 | 1,908,746 | 2,289,405 | 3,512,175 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=ON | 5000 | 2421247.7 | 2,397,589 | 2,410,363 | 2,427,697 | 2,545,198 | 4,029,172 | 5,228,157 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=ON | 5000 | 2977941.3 | 2,957,498 | 2,970,923 | 2,989,818 | 3,062,506 | 3,286,469 | 4,709,457 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=ON | 5000 | 7099.6 | 6,922 | 7,023 | 7,083 | 7,273 | 16,751 | 28,684 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=ON | 5000 | 2058.2 | 1,743 | 1,793 | 3,186 | 3,296 | 10,269 | 22,332 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=ON | 500 | 24162.8 | 23,534 | 23,694 | 23,935 | 36,048 | 57,919 | 57,919 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=ON | 500 | 3171.0 | 2,925 | 2,975 | 2,995 | 4,368 | 64,090 | 64,090 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| cascade | N=100 E=1504 | AVX2/prefetch=OFF | 5000 | 603719.8 | 587,399 | 598,891 | 614,100 | 774,863 | 1,101,610 | 1,451,732 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=OFF | 5000 | 1074108.7 | 1,057,287 | 1,066,114 | 1,096,501 | 1,136,096 | 1,287,392 | 1,940,986 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=OFF | 5000 | 1587013.2 | 1,555,077 | 1,579,203 | 1,615,902 | 1,666,348 | 2,096,430 | 2,905,900 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=OFF | 5000 | 2080587.0 | 2,039,022 | 2,077,735 | 2,113,442 | 2,165,631 | 2,312,448 | 3,321,064 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=OFF | 5000 | 2621647.0 | 2,578,801 | 2,618,296 | 2,652,591 | 2,728,464 | 2,818,915 | 3,011,028 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=OFF | 5000 | 7338.2 | 6,772 | 7,113 | 7,183 | 13,555 | 19,396 | 65,303 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=OFF | 5000 | 2184.9 | 2,093 | 2,144 | 2,173 | 2,234 | 17,442 | 23,434 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=OFF | 500 | 26332.7 | 23,263 | 23,444 | 32,701 | 50,766 | 117,953 | 117,953 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=OFF | 500 | 3161.6 | 2,895 | 2,955 | 2,985 | 6,421 | 67,177 | 67,177 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| clob | fill_levels_1 | AVX2/prefetch=ON | 20000 | 11.6 | 9 | 10 | 20 | 20 | 20 | 160 | depth=256, levels consumed=1 (observed 1), no BBO recording; pinned=1 |
| clob | fill_levels_2 | AVX2/prefetch=ON | 20000 | 11.9 | 9 | 10 | 20 | 20 | 20 | 9,648 | depth=256, levels consumed=2 (observed 2), no BBO recording; pinned=1 |
| clob | fill_levels_4 | AVX2/prefetch=ON | 20000 | 11.7 | 9 | 10 | 20 | 20 | 30 | 80 | depth=256, levels consumed=4 (observed 4), no BBO recording; pinned=1 |
| clob | fill_levels_8 | AVX2/prefetch=ON | 20000 | 15.5 | 9 | 10 | 20 | 20 | 30 | 9,708 | depth=256, levels consumed=8 (observed 8), no BBO recording; pinned=1 |
| clob | fill_levels_16 | AVX2/prefetch=ON | 20000 | 24.3 | 20 | 20 | 30 | 40 | 50 | 9,077 | depth=256, levels consumed=16 (observed 16), no BBO recording; pinned=1 |
| clob | fill_levels_32 | AVX2/prefetch=ON | 20000 | 45.7 | 29 | 49 | 50 | 60 | 89 | 120 | depth=256, levels consumed=32 (observed 32), no BBO recording; pinned=1 |
| clob | fill_levels_64 | AVX2/prefetch=ON | 20000 | 130.2 | 110 | 130 | 130 | 140 | 240 | 17,833 | depth=256, levels consumed=64 (observed 64), no BBO recording; pinned=1 |
| clob | fill_levels_128 | AVX2/prefetch=ON | 20000 | 235.0 | 210 | 230 | 240 | 250 | 370 | 17,102 | depth=256, levels consumed=128 (observed 128), no BBO recording; pinned=1 |
| clob | fill_levels_256 | AVX2/prefetch=ON | 20000 | 419.2 | 390 | 410 | 420 | 471 | 551 | 17,683 | depth=256, levels consumed=256 (observed 256), no BBO recording; pinned=1 |
| clob | fill_depth_8 | AVX2/prefetch=ON | 20000 | 12.6 | 9 | 10 | 20 | 20 | 20 | 14,747 | book depth=8, 1 level consumed; pinned=1 |
| clob | fill_depth_16 | AVX2/prefetch=ON | 20000 | 11.9 | 9 | 10 | 20 | 20 | 20 | 30 | book depth=16, 1 level consumed; pinned=1 |
| clob | fill_depth_32 | AVX2/prefetch=ON | 20000 | 12.0 | 9 | 10 | 20 | 20 | 20 | 80 | book depth=32, 1 level consumed; pinned=1 |
| clob | fill_depth_64 | AVX2/prefetch=ON | 20000 | 11.9 | 9 | 10 | 20 | 20 | 20 | 40 | book depth=64, 1 level consumed; pinned=1 |
| clob | fill_depth_128 | AVX2/prefetch=ON | 20000 | 12.0 | 9 | 10 | 20 | 20 | 20 | 30 | book depth=128, 1 level consumed; pinned=1 |
| clob | fill_depth_256 | AVX2/prefetch=ON | 20000 | 11.9 | 9 | 10 | 20 | 20 | 20 | 150 | book depth=256, 1 level consumed; pinned=1 |
| clob | fill_latency_mixed | AVX2/prefetch=ON | 200000 | 36.2 | 9 | 40 | 60 | 70 | 90 | 12,753 | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=1 |
| clob | bbo_publish_latency | AVX2/prefetch=ON | 100000 | 678919479868033.1 | 7 | 28 | 58 | 68 | 1,971 | 7,543,549,776,311,056,384 | compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=1 |
| noise | fixed_work_unit | AVX2/prefetch=ON | 2000000 | 309.1 | 210 | 310 | 310 | 310 | 340 | 31,289 | identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=ON | 2500000 | 39642854856002008.0 | 7 | 27 | 47 | 1,180 | 7,543,548,267,621,015,552 | 7,543,548,267,621,015,552 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=ON | 2500000 | 7011.2 | 27 | 5,218 | 14,956 | 18,262 | 20,496 | 124,593 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_end_to_end | AVX2/prefetch=ON | 20000 | 11316.3 | 10,598 | 11,058 | 11,409 | 23,632 | 26,738 | 46,756 | 500 nodes/7500 edges; -30% equities on node 0; state restored between events (untimed); paced 300us; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=OFF | 2500000 | 60734622531486640.0 | 7 | 28 | 48 | 1,761 | 7,543,549,102,181,860,352 | 7,543,549,102,181,860,352 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=OFF | 2500000 | 7083.7 | 27 | 5,287 | 14,866 | 18,132 | 20,106 | 154,329 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_end_to_end | AVX2/prefetch=OFF | 20000 | 11163.1 | 10,557 | 10,928 | 11,169 | 23,733 | 26,537 | 55,062 | 500 nodes/7500 edges; -30% equities on node 0; state restored between events (untimed); paced 300us; pinned=1 |

### Scalar metrics

| Bench | Metric | Build | Value | Unit | Conditions |
|---|---|---|---|---|---|
| blackscholes | delta_nonfinite_count | AVX2/prefetch=ON | 0 | count | inputs where the kernel returned NaN/Inf instead of a delta |
| blackscholes | delta_max_abs_err | AVX2/prefetch=ON | 7.40648e-07 | abs | full grid, finite results only |
| blackscholes | delta_mean_abs_err | AVX2/prefetch=ON | 1.39365e-07 | abs | full grid |
| blackscholes | delta_max_abs_err_atm | AVX2/prefetch=ON | 7.07965e-07 | abs | |ln(S/K)|<=0.05 |
| blackscholes | delta_max_abs_err_far | AVX2/prefetch=ON | 7.40648e-07 | abs | |ln(S/K)|>0.20 |
| blackscholes | fastlog_max_abs_err | AVX2/prefetch=ON | 1.17632e-07 | abs | S/K in [0.70, 1.50] |
| blackscholes | rcp_nr_max_rel_err | AVX2/prefetch=ON | 1.64524e-07 | relative | p in [1, 2000] |
| blackscholes | kernel_throughput_ops | AVX2/prefetch=ON | 6.39622e+08 | options/s | single core, batch=496, kernel path=AVX2-8lane-f32 |
| blackscholes | scalar_throughput_ops | AVX2/prefetch=ON | 5.46796e+07 | options/s | std::erfc f64, single core, batch=496 |
| blackscholes | speedup_vs_scalar | AVX2/prefetch=ON | 11.6976 | x | same batch |
| bridge | graph_bytes | AVX2/prefetch=ON | 144448 | bytes | sizeof(CSRGraph) |
| bridge | graph_bytes | AVX2/prefetch=OFF | 144448 | bytes | sizeof(CSRGraph) |
| clob | fill_throughput_fps | AVX2/prefetch=ON | 2.50381e+06 | fills/s | includes refresh_liquidity re-quote per fill; pinned=1 |
| noise | p99_over_p50 | AVX2/prefetch=ON | 1 | ratio | 1.00 = interference does not reach p99 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=ON | 12.9477 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=ON | 2.57816 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=ON | 4.69287e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=ON | 4.62139e+06 | events/s | unpaced; mutex+queue; pinned=1 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=OFF | 764.75 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=OFF | 2.35841 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=OFF | 4.69148e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=OFF | 4.60936e+06 | events/s | unpaced; mutex+queue; pinned=1 |

## 5. Reproducing

```sh
./bench/run_all.sh          # build both variants, run everything, regenerate this file
./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness
```

Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, `bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.

