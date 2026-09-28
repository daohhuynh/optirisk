# OptiRisk — Measured Benchmarks

Generated 2026-09-28 09:55:01 UTC by `bench/run_all.sh` + `bench/make_report.py`.

Every number below is a measurement. Nothing here is estimated, extrapolated,
rounded up, or carried over from another machine. Where a measurement could not
be taken, the row says so rather than being omitted.


## 1. Hardware and toolchain

```
OptiRisk benchmark environment
generated: 2026-09-28T09:52:44Z
git commit: d231b1e
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

**Timer resolution:** `effective timer granularity: 7 ns (smallest nonzero back-to-back delta)`

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
| Ring handoff latency, p50 | 27 ns | 2,589 ns (mutex+queue) | 95.89x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99 | 43 ns | 7,967 ns (mutex+queue) | 185.28x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99.9 | 722 ns | 118,912 ns (mutex+queue) | 164.70x | SPSC 1024-slot, paced to queue depth ~1 |
| Transport throughput | 43,104,900 events/s | 4,414,230 events/s (mutex+queue) | 9.76x | producer unpaced (saturated) |
| Cascade tick (real 500n/7500e), p50 | 8,828 ns | — | — | real 500 nodes/7500 edges; -30% equities on node 0; state restored per |
| Cascade tick (real 500n/7500e), p99 | 10,888 ns | — | — | same run |
| Prefetch ablation (cascade p50) | 8,828 ns (ON) | 8,927 ns (OFF) | 1.01x | same source, two builds |
| SIMD phases only, p50 | 2,074 ns | 2,047 ns (prefetch OFF) | 0.99x | apply_shock_simd, 500 nodes |
| Option kernel throughput | 840.51 M options/s | 57.82 M options/s (std::erfc f64) | 14.54x | single core, batch=496, kernel path=AVX2-8lane-f32 |
| Delta max abs error | 8.003e-07 | double-precision reference | — | full grid, finite results only |
| Delta mean abs error | 1.396e-07 | double-precision reference | — | full grid |
| Fast-log max abs error | 1.176e-07 | double-precision reference | — | S/K in [0.70, 1.50] |
| rcp+NR max rel error | 1.618e-07 | double-precision reference | — | p in [1, 2000] |
| CLOB fill latency, p50 | 64 ns | — | — | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fi |
| CLOB fill latency, p99 | 138 ns | — | — | same run |
| CLOB fill throughput | 1,262,670 fills/s | — | — | includes refresh_liquidity re-quote per fill; pinned=1 |
| BBO publish -> observed, p50 | 36 ns | — | — | compute stamps then flip_buffers(); reader spins on active_buffer_idx  |
| BBO publish -> observed, p99 | 53 ns | — | — | same run |
| Python->C++ graph load (C++ side) | 15,127 ns | 3,470 ns (in-process memcpy floor) | 4.36x SLOWER | fopen + 21 fread, warm page cache |

### 3.1 `O(levels consumed)` — cost vs levels consumed

| Levels consumed | p50 (ns) | p99 (ns) | ns per level |
|---|---|---|---|
| 1 | 17 | 22 | 17.00 |
| 2 | 18 | 23 | 9.00 |
| 4 | 20 | 25 | 5.00 |
| 8 | 24 | 29 | 3.00 |
| 16 | 32 | 38 | 2.00 |
| 32 | 68 | 76 | 2.12 |
| 64 | 148 | 156 | 2.31 |
| 128 | 306 | 351 | 2.39 |
| 256 | 660 | 673 | 2.58 |

### 3.2 `O(levels consumed)` — cost vs book depth (1 level consumed)

Flat here is the claim. Growth would mean the book shifts memory on consumption.

| Book depth | p50 (ns) | p99 (ns) |
|---|---|---|
| 8 | 17 | 22 |
| 16 | 17 | 22 |
| 32 | 17 | 22 |
| 64 | 17 | 22 |
| 128 | 17 | 22 |
| 256 | 17 | 22 |

### 3.3 Cascade cost by graph size

| Graph | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|
| N=100 E=1504 | 664,313 | 758,766 | 898,492 | 898,492 |
| N=200 E=3004 | 1,262,821 | 1,300,609 | 1,330,147 | 1,330,147 |
| N=300 E=4483 | 1,842,407 | 1,888,146 | 2,065,594 | 2,065,594 |
| N=400 E=5994 | 2,409,378 | 2,508,125 | 3,346,587 | 3,346,587 |
| N=500 E=7475 | 2,978,412 | 3,073,409 | 3,154,063 | 3,154,063 |
| N=100 E=1504 | 604,909 | 629,056 | 671,668 | 671,668 |
| N=200 E=3004 | 1,117,878 | 1,169,736 | 1,593,178 | 1,593,178 |
| N=300 E=4483 | 1,632,403 | 1,686,406 | 1,714,096 | 1,714,096 |
| N=400 E=5994 | 2,148,741 | 2,231,847 | 2,340,597 | 2,340,597 |
| N=500 E=7475 | 2,716,112 | 2,772,367 | 2,818,778 | 2,818,778 |

### 3.4 Branchless claim — disassembly

`bench/results/blackscholes_disasm.txt`: **12 conditional branches in probe_black_scholes**

A nonzero count means the hot loop is not branchless even where the arithmetic inside it is.


### 3.5 Python-side handoff

| Mechanism | p50 (ns) | p99 (ns) | bytes |
|---|---|---|---|
| tobytes_file_write | 399,341 | 3,019,271 | 143820 |
| pickle_write | 405,078 | 2,791,047 | 144737 |
| pickle_read | 39,217 | 74,845 | — |
| shared_memory_write | 20,533 | 41,816 | 143808 |

## 4. Full percentile output

Complete per-metric percentiles, exactly as emitted:

| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cascade | N=100 E=1504 | AVX2/prefetch=ON | 400 | 667389.4 | 650,877 | 664,313 | 672,969 | 758,766 | 898,492 | 898,492 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=ON | 400 | 1263706.3 | 1,236,098 | 1,262,821 | 1,275,667 | 1,300,609 | 1,330,147 | 1,330,147 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=ON | 400 | 1844341.8 | 1,809,709 | 1,842,407 | 1,862,141 | 1,888,146 | 2,065,594 | 2,065,594 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=ON | 400 | 2415497.7 | 2,365,017 | 2,409,378 | 2,435,716 | 2,508,125 | 3,346,587 | 3,346,587 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=ON | 400 | 2981341.2 | 2,867,864 | 2,978,412 | 3,009,432 | 3,073,409 | 3,154,063 | 3,154,063 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=ON | 5000 | 8966.5 | 8,652 | 8,828 | 9,108 | 10,888 | 15,510 | 21,128 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=ON | 5000 | 2120.9 | 2,023 | 2,074 | 2,100 | 2,346 | 12,011 | 14,447 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=ON | 500 | 15295.7 | 14,954 | 15,127 | 15,215 | 22,122 | 36,882 | 36,882 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=ON | 500 | 3621.6 | 3,358 | 3,470 | 3,509 | 3,540 | 65,155 | 65,155 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| cascade | N=100 E=1504 | AVX2/prefetch=OFF | 400 | 605333.2 | 589,282 | 604,909 | 612,029 | 629,056 | 671,668 | 671,668 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=OFF | 400 | 1120480.3 | 1,091,235 | 1,117,878 | 1,130,396 | 1,169,736 | 1,593,178 | 1,593,178 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=OFF | 400 | 1633974.5 | 1,601,961 | 1,632,403 | 1,650,287 | 1,686,406 | 1,714,096 | 1,714,096 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=OFF | 400 | 2152916.0 | 2,110,569 | 2,148,741 | 2,176,371 | 2,231,847 | 2,340,597 | 2,340,597 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=OFF | 400 | 2718598.0 | 2,667,193 | 2,716,112 | 2,744,690 | 2,772,367 | 2,818,778 | 2,818,778 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=OFF | 5000 | 9068.8 | 8,567 | 8,927 | 9,182 | 11,161 | 16,778 | 28,001 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=OFF | 5000 | 2340.8 | 1,986 | 2,047 | 3,089 | 3,360 | 13,015 | 15,157 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=OFF | 500 | 15284.3 | 15,001 | 15,109 | 15,224 | 21,861 | 28,423 | 28,423 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=OFF | 500 | 3622.4 | 3,341 | 3,480 | 3,509 | 3,547 | 60,840 | 60,840 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| clob | fill_levels_1 | AVX2/prefetch=ON | 20000 | 17.0 | 10 | 17 | 21 | 22 | 23 | 80 | depth=256, levels consumed=1 (observed 1), no BBO recording; pinned=1 |
| clob | fill_levels_2 | AVX2/prefetch=ON | 20000 | 19.2 | 12 | 18 | 22 | 23 | 26 | 13,485 | depth=256, levels consumed=2 (observed 2), no BBO recording; pinned=1 |
| clob | fill_levels_4 | AVX2/prefetch=ON | 20000 | 20.5 | 13 | 20 | 24 | 25 | 26 | 10,734 | depth=256, levels consumed=4 (observed 4), no BBO recording; pinned=1 |
| clob | fill_levels_8 | AVX2/prefetch=ON | 20000 | 24.8 | 18 | 24 | 28 | 29 | 30 | 9,734 | depth=256, levels consumed=8 (observed 8), no BBO recording; pinned=1 |
| clob | fill_levels_16 | AVX2/prefetch=ON | 20000 | 32.4 | 25 | 32 | 36 | 38 | 39 | 100 | depth=256, levels consumed=16 (observed 16), no BBO recording; pinned=1 |
| clob | fill_levels_32 | AVX2/prefetch=ON | 20000 | 70.0 | 60 | 68 | 73 | 76 | 78 | 12,090 | depth=256, levels consumed=32 (observed 32), no BBO recording; pinned=1 |
| clob | fill_levels_64 | AVX2/prefetch=ON | 20000 | 150.4 | 138 | 148 | 153 | 156 | 177 | 11,544 | depth=256, levels consumed=64 (observed 64), no BBO recording; pinned=1 |
| clob | fill_levels_128 | AVX2/prefetch=ON | 20000 | 311.1 | 295 | 306 | 313 | 351 | 360 | 13,936 | depth=256, levels consumed=128 (observed 128), no BBO recording; pinned=1 |
| clob | fill_levels_256 | AVX2/prefetch=ON | 20000 | 663.9 | 642 | 660 | 666 | 673 | 727 | 10,868 | depth=256, levels consumed=256 (observed 256), no BBO recording; pinned=1 |
| clob | fill_depth_8 | AVX2/prefetch=ON | 20000 | 17.4 | 11 | 17 | 21 | 22 | 22 | 65 | book depth=8, 1 level consumed; pinned=1 |
| clob | fill_depth_16 | AVX2/prefetch=ON | 20000 | 17.3 | 11 | 17 | 21 | 22 | 22 | 49 | book depth=16, 1 level consumed; pinned=1 |
| clob | fill_depth_32 | AVX2/prefetch=ON | 20000 | 17.4 | 11 | 17 | 21 | 22 | 22 | 40 | book depth=32, 1 level consumed; pinned=1 |
| clob | fill_depth_64 | AVX2/prefetch=ON | 20000 | 17.4 | 11 | 17 | 21 | 22 | 22 | 39 | book depth=64, 1 level consumed; pinned=1 |
| clob | fill_depth_128 | AVX2/prefetch=ON | 20000 | 17.7 | 12 | 17 | 21 | 22 | 23 | 6,834 | book depth=128, 1 level consumed; pinned=1 |
| clob | fill_depth_256 | AVX2/prefetch=ON | 20000 | 17.4 | 12 | 17 | 21 | 22 | 22 | 40 | book depth=256, 1 level consumed; pinned=1 |
| clob | fill_latency_mixed | AVX2/prefetch=ON | 200000 | 66.5 | 14 | 64 | 110 | 138 | 156 | 11,267 | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=1 |
| clob | bbo_publish_latency | AVX2/prefetch=ON | 100000 | 38.1 | 10 | 36 | 48 | 53 | 62 | 9,744 | compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=1 |
| clob | bbo_multicast_sendmsg | AVX2/prefetch=ON | 20000 | 4177.2 | 2,314 | 4,154 | 4,889 | 5,694 | 16,395 | 315,057 | sendmsg to 239.255.0.1:9090, 9 BboUpdate entries (144 bytes) via 2-entry iovec; pinned=1 |
| noise | fixed_work_unit | AVX2/prefetch=ON | 2000000 | 232.9 | 214 | 232 | 239 | 245 | 360 | 22,882 | identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=ON | 2500000 | 66.8 | 10 | 27 | 39 | 43 | 722 | 327,632 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=ON | 2500000 | 3685.6 | 59 | 2,589 | 4,803 | 7,967 | 118,912 | 2,576,095 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_no_defaults | AVX2/prefetch=ON | 400 | 11174.5 | 10,506 | 11,149 | 11,467 | 12,983 | 14,296 | 14,296 | no defaults: 500 nodes/7500 edges, 2 rounds, 0 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_small_cascade | AVX2/prefetch=ON | 400 | 4391746.5 | 4,259,424 | 4,385,177 | 4,457,456 | 4,526,047 | 4,585,229 | 4,585,229 | small cascade: 500 nodes/7500 edges, 1024 rounds, 22 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_large_cascade | AVX2/prefetch=ON | 400 | 6280500.5 | 6,086,481 | 6,256,616 | 6,418,329 | 6,587,607 | 6,613,950 | 6,613,950 | large cascade: 500 nodes/7500 edges, 1024 rounds, 215 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=OFF | 2500000 | 3208187708695.8 | 10 | 27 | 39 | 43 | 671 | 8,020,469,271,578,952,704 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=OFF | 2500000 | 2840.1 | 57 | 2,663 | 4,942 | 8,021 | 12,876 | 315,207 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_no_defaults | AVX2/prefetch=OFF | 400 | 11032.3 | 10,396 | 10,879 | 11,208 | 15,210 | 33,934 | 33,934 | no defaults: 500 nodes/7500 edges, 2 rounds, 0 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_small_cascade | AVX2/prefetch=OFF | 400 | 4294020.3 | 4,198,339 | 4,290,480 | 4,348,544 | 4,432,862 | 4,483,445 | 4,483,445 | small cascade: 500 nodes/7500 edges, 1024 rounds, 22 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_large_cascade | AVX2/prefetch=OFF | 400 | 6277115.2 | 6,084,880 | 6,246,615 | 6,473,927 | 6,605,631 | 6,716,698 | 6,716,698 | large cascade: 500 nodes/7500 edges, 1024 rounds, 215 defaults; state restored between events (untimed); paced 20000us; pinned=1 |

### Scalar metrics

| Bench | Metric | Build | Value | Unit | Conditions |
|---|---|---|---|---|---|
| blackscholes | delta_nonfinite_count | AVX2/prefetch=ON | 0 | count | inputs where the kernel returned NaN/Inf instead of a delta |
| blackscholes | delta_max_abs_err | AVX2/prefetch=ON | 8.00252e-07 | abs | full grid, finite results only |
| blackscholes | delta_mean_abs_err | AVX2/prefetch=ON | 1.39572e-07 | abs | full grid |
| blackscholes | delta_max_abs_err_atm | AVX2/prefetch=ON | 7.07965e-07 | abs | |ln(S/K)|<=0.05 |
| blackscholes | delta_max_abs_err_far | AVX2/prefetch=ON | 8.00252e-07 | abs | |ln(S/K)|>0.20 |
| blackscholes | unpriced_tail_options | AVX2/prefetch=ON | 0 | count | count=500; options the kernel never wrote |
| blackscholes | fastlog_max_abs_err | AVX2/prefetch=ON | 1.17632e-07 | abs | S/K in [0.70, 1.50] |
| blackscholes | rcp_nr_max_rel_err | AVX2/prefetch=ON | 1.61759e-07 | relative | p in [1, 2000] |
| blackscholes | f32_scalar_vs_avx2_max_diff | AVX2/prefetch=ON | 0 | abs | scalar f32 approx vs AVX2 kernel, same inputs |
| blackscholes | f32_scalar_throughput_ops | AVX2/prefetch=ON | 1.00361e+08 | options/s | scalar f32, same approximations as the AVX2 kernel, single lane |
| blackscholes | speedup_math_only | AVX2/prefetch=ON | 1.73571 | x | f64 libm -> f32 approx, both scalar |
| blackscholes | speedup_vectorization_only | AVX2/prefetch=ON | 8.37489 | x | f32 approx scalar -> AVX2 8-lane |
| blackscholes | kernel_throughput_ops | AVX2/prefetch=ON | 8.40512e+08 | options/s | single core, batch=496, kernel path=AVX2-8lane-f32 |
| blackscholes | scalar_throughput_ops | AVX2/prefetch=ON | 5.78213e+07 | options/s | std::erfc f64, single core, batch=496 |
| blackscholes | speedup_vs_scalar | AVX2/prefetch=ON | 14.5364 | x | same batch |
| bridge | graph_bytes | AVX2/prefetch=ON | 144448 | bytes | sizeof(CSRGraph) |
| bridge | graph_bytes | AVX2/prefetch=OFF | 144448 | bytes | sizeof(CSRGraph) |
| clob | fill_throughput_fps | AVX2/prefetch=ON | 1.26267e+06 | fills/s | includes refresh_liquidity re-quote per fill; pinned=1 |
| convergence | -10% equities | AVX2/prefetch=ON | 1 | rounds | -10% equities: new 1 rounds / legacy 3 rounds; 0 defaults; DIVERGED |
| convergence_legacy | -10% equities | AVX2/prefetch=ON | 3 | rounds | -10% equities: new 1 rounds / legacy 3 rounds; 0 defaults; DIVERGED |
| convergence | -30% equities | AVX2/prefetch=ON | 1 | rounds | -30% equities: new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence_legacy | -30% equities | AVX2/prefetch=ON | 2 | rounds | -30% equities: new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence | -50% equities | AVX2/prefetch=ON | 1 | rounds | -50% equities: new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence_legacy | -50% equities | AVX2/prefetch=ON | 2 | rounds | -50% equities: new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence | -80% equities | AVX2/prefetch=ON | 1 | rounds | -80% equities: new 1 rounds / legacy 1024 rounds; 0 defaults; DIVERGED |
| convergence_legacy | -80% equities | AVX2/prefetch=ON | 1024 | rounds | -80% equities: new 1 rounds / legacy 1024 rounds; 0 defaults; DIVERGED |
| convergence | -80% crypto | AVX2/prefetch=ON | 1 | rounds | -80% crypto: new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence_legacy | -80% crypto | AVX2/prefetch=ON | 2 | rounds | -80% crypto: new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence | Lehman (-40/-25/-15) | AVX2/prefetch=ON | 1 | rounds | Lehman (-40/-25/-15): new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence_legacy | Lehman (-40/-25/-15) | AVX2/prefetch=ON | 2 | rounds | Lehman (-40/-25/-15): new 1 rounds / legacy 2 rounds; 0 defaults; final state identical |
| convergence | Covid (-35/-10/-50) | AVX2/prefetch=ON | 1 | rounds | Covid (-35/-10/-50): new 1 rounds / legacy 1024 rounds; 0 defaults; DIVERGED |
| convergence_legacy | Covid (-35/-10/-50) | AVX2/prefetch=ON | 1024 | rounds | Covid (-35/-10/-50): new 1 rounds / legacy 1024 rounds; 0 defaults; DIVERGED |
| convergence | -50% all classes | AVX2/prefetch=ON | 3 | rounds | -50% all classes: new 3 rounds / legacy 1024 rounds; 212 defaults; DIVERGED |
| convergence_legacy | -50% all classes | AVX2/prefetch=ON | 1024 | rounds | -50% all classes: new 3 rounds / legacy 1024 rounds; 212 defaults; DIVERGED |
| convergence | -90% all classes | AVX2/prefetch=ON | 2 | rounds | -90% all classes: new 2 rounds / legacy 2 rounds; 500 defaults; final state identical |
| convergence_legacy | -90% all classes | AVX2/prefetch=ON | 2 | rounds | -90% all classes: new 2 rounds / legacy 2 rounds; 500 defaults; final state identical |
| convergence | diverged_scenarios | AVX2/prefetch=ON | 4 | count | shocks whose final state differs between the two policies |
| noise | p99_over_p50 | AVX2/prefetch=ON | 1.05603 | ratio | 1.00 = interference does not reach p99 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=ON | 0 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=ON | 3.17178 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=ON | 4.31049e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=ON | 4.41423e+06 | events/s | unpaced; mutex+queue; pinned=1 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=OFF | 0 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=OFF | 1.72175 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=OFF | 4.30173e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=OFF | 4.50038e+06 | events/s | unpaced; mutex+queue; pinned=1 |

## 4.6 Where the option kernel speedup comes from

The headline figure compares scalar f64 libm against AVX2 f32, which changes
two things at once. Adding a scalar f32 rung that uses the kernel's exact
approximations (same fast-log series, same Horner order, same y saturation,
same 14-bit reciprocal plus one Newton-Raphson step) separates them. All
three run back to back on one thread, confirmed single-threaded by reading
/proc/self/status, over the same 496-option batch.

| Variant | M options/s | ns/option |
|---|---|---|
| A. scalar f64, libm `std::erfc` | 57.8 | 17.29 |
| B. scalar f32, same approximations | 100.4 | 9.96 |
| C. AVX2 f32, 8 lanes (shipped) | 840.5 | 1.19 |

| Contribution | Factor | What changed |
|---|---|---|
| Cheaper math (A to B) | **1.74x** | f64 libm to f32 polynomial approximations |
| Vectorization (B to C) | **8.37x** | 1 lane to 8 lanes |
| Total (A to C) | **14.54x** | |

**Vectorization is doing nearly all of the work.** The approximations are
worth 1.74x; the eight lanes are worth 8.37x, slightly above the 8.00x lane
ceiling because the vector form also amortises loop overhead across a group.
B and C agree to 0.000e+00 max absolute difference, so the split compares two
implementations of the same function rather than two different functions.

## 4.7 BBO: in-process visibility is not the wire

Two different costs, often conflated:

| Stage | p50 | p99 | Transport |
|---|---|---|---|
| Flip to in-process reader observes | **36 ns** | 53 ns | Two threads, one process, shared ping-pong buffer. No socket. |
| `broadcast_bbo` sendmsg to multicast | **4,154 ns** | 5,694 ns | `sendmsg` to 239.255.0.1:9090, 2-entry iovec, 9 entries / 144 bytes |

The 36 ns is a cross-core cache-line transfer, not a publish. The shipped
path in `main.cpp`'s broadcast_thread does both: it reads the inactive buffer
exactly as the benchmark's reader does, then hands the span to
`UdpPublisher::broadcast_bbo()`. Fill-to-wire is therefore roughly 4.2 us,
dominated entirely by the syscall, and the ping-pong buffer contributes under
1% of it.

UDP multicast appears in exactly one place in the project: `UdpPublisher`
(`backend/src/network/udp_publisher.hpp`), constructed in `main.cpp` as
`{"239.255.0.1", 9090}` and called from the broadcast thread via
`broadcast_tick`, `broadcast_bbo`, and `broadcast_var`. The browser frontend
cannot receive multicast, so it consumes the WebSocket path instead; the UDP
feed has no consumer in this repository.

## 5. Reproducing

```sh
./bench/run_all.sh          # build both variants, run everything, regenerate this file
./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness
```

Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, `bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.

