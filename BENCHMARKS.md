# OptiRisk — Measured Benchmarks

Generated 2026-09-28 09:24:32 UTC by `bench/run_all.sh` + `bench/make_report.py`.

Every number below is a measurement. Nothing here is estimated, extrapolated,
rounded up, or carried over from another machine. Where a measurement could not
be taken, the row says so rather than being omitted.


## 1. Hardware and toolchain

```
OptiRisk benchmark environment
generated: 2026-09-28T09:22:13Z
git commit: a7afcb4
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
| Ring handoff latency, p50 | 42 ns | 5,542 ns (mutex+queue) | 131.95x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99 | 893 ns | 17,925 ns (mutex+queue) | 20.07x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99.9 | 2,196 ns | 20,039 ns (mutex+queue) | 9.13x | SPSC 1024-slot, paced to queue depth ~1 |
| Transport throughput | 47,273,700 events/s | 4,515,190 events/s (mutex+queue) | 10.47x | producer unpaced (saturated) |
| Cascade tick (real 500n/7500e), p50 | 7,594 ns | — | — | real 500 nodes/7500 edges; -30% equities on node 0; state restored per |
| Cascade tick (real 500n/7500e), p99 | 16,340 ns | — | — | same run |
| Prefetch ablation (cascade p50) | 7,594 ns (ON) | 6,702 ns (OFF) | 0.88x | same source, two builds |
| SIMD phases only, p50 | 1,793 ns | 2,133 ns (prefetch OFF) | 1.19x | apply_shock_simd, 500 nodes |
| Option kernel throughput | 688.67 M options/s | 54.68 M options/s (std::erfc f64) | 12.60x | single core, batch=496, kernel path=AVX2-8lane-f32 |
| Delta max abs error | 7.406e-07 | double-precision reference | — | full grid, finite results only |
| Delta mean abs error | 1.394e-07 | double-precision reference | — | full grid |
| Fast-log max abs error | 1.176e-07 | double-precision reference | — | S/K in [0.70, 1.50] |
| rcp+NR max rel error | 1.645e-07 | double-precision reference | — | p in [1, 2000] |
| CLOB fill latency, p50 | 40 ns | — | — | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fi |
| CLOB fill latency, p99 | 70 ns | — | — | same run |
| CLOB fill throughput | 2,488,030 fills/s | — | — | includes refresh_liquidity re-quote per fill; pinned=1 |
| BBO publish -> observed, p50 | 42 ns | — | — | compute stamps then flip_buffers(); reader spins on active_buffer_idx  |
| BBO publish -> observed, p99 | 82 ns | — | — | same run |
| Python->C++ graph load (C++ side) | 23,824 ns | 2,885 ns (in-process memcpy floor) | 8.26x SLOWER | fopen + 21 fread, warm page cache |

### 3.1 `O(levels consumed)` — cost vs levels consumed

| Levels consumed | p50 (ns) | p99 (ns) | ns per level |
|---|---|---|---|
| 1 | 10 | 20 | 10.00 |
| 2 | 10 | 20 | 5.00 |
| 4 | 10 | 20 | 2.50 |
| 8 | 20 | 20 | 2.50 |
| 16 | 20 | 40 | 1.25 |
| 32 | 49 | 60 | 1.53 |
| 64 | 130 | 140 | 2.03 |
| 128 | 230 | 240 | 1.80 |
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
| N=100 E=1504 | 698,182 | 753,857 | 935,879 | 935,879 |
| N=200 E=3004 | 1,249,538 | 1,300,845 | 1,338,045 | 1,338,045 |
| N=300 E=4483 | 1,860,307 | 1,930,138 | 2,071,233 | 2,071,233 |
| N=400 E=5994 | 2,408,758 | 4,217,198 | 4,306,816 | 4,306,816 |
| N=500 E=7475 | 2,986,755 | 3,112,751 | 3,539,313 | 3,539,313 |
| N=100 E=1504 | 621,257 | 664,769 | 698,893 | 698,893 |
| N=200 E=3004 | 1,078,026 | 1,142,346 | 1,160,681 | 1,160,681 |
| N=300 E=4483 | 1,610,216 | 1,685,617 | 1,814,349 | 1,814,349 |
| N=400 E=5994 | 2,091,521 | 2,182,241 | 2,291,075 | 2,291,075 |
| N=500 E=7475 | 2,594,916 | 2,671,831 | 2,745,740 | 2,745,740 |

### 3.4 Branchless claim — disassembly

`bench/results/blackscholes_disasm.txt`: **12 conditional branches in probe_black_scholes**

A nonzero count means the hot loop is not branchless even where the arithmetic inside it is.


### 3.5 Python-side handoff

| Mechanism | p50 (ns) | p99 (ns) | bytes |
|---|---|---|---|
| tobytes_file_write | 375,028 | 628,219 | 143820 |
| pickle_write | 394,755 | 587,699 | 144737 |
| pickle_read | 65,873 | 101,099 | — |
| shared_memory_write | 28,453 | 51,676 | 143808 |

## 4. Full percentile output

Complete per-metric percentiles, exactly as emitted:

| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cascade | N=100 E=1504 | AVX2/prefetch=ON | 400 | 698926.5 | 688,013 | 698,182 | 703,633 | 753,857 | 935,879 | 935,879 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=ON | 400 | 1253217.5 | 1,244,399 | 1,249,538 | 1,260,960 | 1,300,845 | 1,338,045 | 1,338,045 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=ON | 400 | 1866273.1 | 1,848,535 | 1,860,307 | 1,886,226 | 1,930,138 | 2,071,233 | 2,071,233 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=ON | 400 | 2453651.6 | 2,388,149 | 2,408,758 | 2,442,642 | 4,217,198 | 4,306,816 | 4,306,816 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=ON | 400 | 2998182.8 | 2,972,087 | 2,986,755 | 3,020,608 | 3,112,751 | 3,539,313 | 3,539,313 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=ON | 5000 | 7947.1 | 7,203 | 7,594 | 7,664 | 16,340 | 97,994 | 110,377 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=ON | 5000 | 2056.2 | 1,733 | 1,793 | 3,196 | 3,346 | 9,938 | 21,861 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=ON | 500 | 24312.4 | 23,634 | 23,824 | 24,376 | 35,326 | 48,551 | 48,551 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=ON | 500 | 3058.2 | 2,835 | 2,885 | 2,905 | 3,105 | 69,410 | 69,410 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| cascade | N=100 E=1504 | AVX2/prefetch=OFF | 400 | 623569.8 | 609,846 | 621,257 | 635,965 | 664,769 | 698,893 | 698,893 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=OFF | 400 | 1084734.3 | 1,073,828 | 1,078,026 | 1,105,397 | 1,142,346 | 1,160,681 | 1,160,681 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=OFF | 400 | 1616950.6 | 1,586,351 | 1,610,216 | 1,649,329 | 1,685,617 | 1,814,349 | 1,814,349 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=OFF | 400 | 2093576.5 | 2,049,131 | 2,091,521 | 2,130,864 | 2,182,241 | 2,291,075 | 2,291,075 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=OFF | 400 | 2596811.7 | 2,552,777 | 2,594,916 | 2,631,996 | 2,671,831 | 2,745,740 | 2,745,740 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=OFF | 5000 | 6788.5 | 6,612 | 6,702 | 6,752 | 9,157 | 17,342 | 25,578 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=OFF | 5000 | 2334.7 | 2,093 | 2,133 | 3,116 | 3,226 | 12,794 | 33,252 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=OFF | 500 | 24247.9 | 23,584 | 23,784 | 24,355 | 35,977 | 49,302 | 49,302 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=OFF | 500 | 3038.8 | 2,845 | 2,885 | 2,905 | 3,115 | 67,707 | 67,707 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| clob | fill_levels_1 | AVX2/prefetch=ON | 20000 | 11.3 | 9 | 10 | 20 | 20 | 20 | 30 | depth=256, levels consumed=1 (observed 1), no BBO recording; pinned=1 |
| clob | fill_levels_2 | AVX2/prefetch=ON | 20000 | 11.2 | 9 | 10 | 20 | 20 | 20 | 50 | depth=256, levels consumed=2 (observed 2), no BBO recording; pinned=1 |
| clob | fill_levels_4 | AVX2/prefetch=ON | 20000 | 11.7 | 9 | 10 | 20 | 20 | 30 | 170 | depth=256, levels consumed=4 (observed 4), no BBO recording; pinned=1 |
| clob | fill_levels_8 | AVX2/prefetch=ON | 20000 | 15.6 | 9 | 20 | 20 | 20 | 40 | 90 | depth=256, levels consumed=8 (observed 8), no BBO recording; pinned=1 |
| clob | fill_levels_16 | AVX2/prefetch=ON | 20000 | 27.0 | 20 | 20 | 30 | 40 | 50 | 15,549 | depth=256, levels consumed=16 (observed 16), no BBO recording; pinned=1 |
| clob | fill_levels_32 | AVX2/prefetch=ON | 20000 | 45.5 | 29 | 49 | 50 | 60 | 80 | 360 | depth=256, levels consumed=32 (observed 32), no BBO recording; pinned=1 |
| clob | fill_levels_64 | AVX2/prefetch=ON | 20000 | 129.6 | 120 | 130 | 130 | 140 | 200 | 16,701 | depth=256, levels consumed=64 (observed 64), no BBO recording; pinned=1 |
| clob | fill_levels_128 | AVX2/prefetch=ON | 20000 | 235.2 | 220 | 230 | 240 | 240 | 310 | 15,339 | depth=256, levels consumed=128 (observed 128), no BBO recording; pinned=1 |
| clob | fill_levels_256 | AVX2/prefetch=ON | 20000 | 419.0 | 390 | 410 | 420 | 471 | 561 | 16,330 | depth=256, levels consumed=256 (observed 256), no BBO recording; pinned=1 |
| clob | fill_depth_8 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 40 | book depth=8, 1 level consumed; pinned=1 |
| clob | fill_depth_16 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 170 | book depth=16, 1 level consumed; pinned=1 |
| clob | fill_depth_32 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 160 | book depth=32, 1 level consumed; pinned=1 |
| clob | fill_depth_64 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 89 | book depth=64, 1 level consumed; pinned=1 |
| clob | fill_depth_128 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 180 | book depth=128, 1 level consumed; pinned=1 |
| clob | fill_depth_256 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 80 | book depth=256, 1 level consumed; pinned=1 |
| clob | fill_latency_mixed | AVX2/prefetch=ON | 200000 | 37.1 | 9 | 40 | 60 | 70 | 90 | 35,707 | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=1 |
| clob | bbo_publish_latency | AVX2/prefetch=ON | 100000 | 52.3 | 12 | 42 | 72 | 82 | 1,885 | 16,132 | compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=1 |
| noise | fixed_work_unit | AVX2/prefetch=ON | 2000000 | 309.1 | 210 | 310 | 310 | 310 | 340 | 39,454 | identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=ON | 2500000 | 3017385912981.4 | 2 | 42 | 62 | 893 | 2,196 | 7,543,464,782,278,547,456 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=ON | 2500000 | 7172.5 | 42 | 5,542 | 14,770 | 17,925 | 20,039 | 248,619 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_no_defaults | AVX2/prefetch=ON | 400 | 19021.3 | 17,274 | 18,847 | 20,210 | 22,634 | 23,135 | 23,135 | no defaults: 500 nodes/7500 edges, 2 rounds, 0 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_small_cascade | AVX2/prefetch=ON | 400 | 5235684.9 | 5,142,800 | 5,240,363 | 5,255,281 | 5,268,246 | 5,293,944 | 5,293,944 | small cascade: 500 nodes/7500 edges, 1024 rounds, 22 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_large_cascade | AVX2/prefetch=ON | 400 | 7895933.6 | 7,806,919 | 7,893,972 | 7,938,336 | 7,977,950 | 8,022,063 | 8,022,063 | large cascade: 500 nodes/7500 edges, 1024 rounds, 215 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=OFF | 2500000 | 63.4 | 12 | 42 | 62 | 102 | 1,805 | 136,608 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=OFF | 2500000 | 7234.9 | 32 | 5,562 | 14,870 | 17,995 | 19,929 | 84,891 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_no_defaults | AVX2/prefetch=OFF | 400 | 18395.0 | 15,852 | 18,166 | 19,999 | 23,385 | 24,468 | 24,468 | no defaults: 500 nodes/7500 edges, 2 rounds, 0 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_small_cascade | AVX2/prefetch=OFF | 400 | 5372157.3 | 5,106,887 | 5,373,567 | 5,386,812 | 5,401,189 | 5,699,087 | 5,699,087 | small cascade: 500 nodes/7500 edges, 1024 rounds, 22 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_large_cascade | AVX2/prefetch=OFF | 400 | 8198562.1 | 8,111,450 | 8,195,758 | 8,250,361 | 8,284,164 | 8,530,847 | 8,530,847 | large cascade: 500 nodes/7500 edges, 1024 rounds, 215 defaults; state restored between events (untimed); paced 20000us; pinned=1 |

### Scalar metrics

| Bench | Metric | Build | Value | Unit | Conditions |
|---|---|---|---|---|---|
| blackscholes | delta_nonfinite_count | AVX2/prefetch=ON | 0 | count | inputs where the kernel returned NaN/Inf instead of a delta |
| blackscholes | delta_max_abs_err | AVX2/prefetch=ON | 7.40648e-07 | abs | full grid, finite results only |
| blackscholes | delta_mean_abs_err | AVX2/prefetch=ON | 1.39365e-07 | abs | full grid |
| blackscholes | delta_max_abs_err_atm | AVX2/prefetch=ON | 7.07965e-07 | abs | |ln(S/K)|<=0.05 |
| blackscholes | delta_max_abs_err_far | AVX2/prefetch=ON | 7.40648e-07 | abs | |ln(S/K)|>0.20 |
| blackscholes | unpriced_tail_options | AVX2/prefetch=ON | 0 | count | count=500; options the kernel never wrote |
| blackscholes | fastlog_max_abs_err | AVX2/prefetch=ON | 1.17632e-07 | abs | S/K in [0.70, 1.50] |
| blackscholes | rcp_nr_max_rel_err | AVX2/prefetch=ON | 1.64524e-07 | relative | p in [1, 2000] |
| blackscholes | kernel_throughput_ops | AVX2/prefetch=ON | 6.88669e+08 | options/s | single core, batch=496, kernel path=AVX2-8lane-f32 |
| blackscholes | scalar_throughput_ops | AVX2/prefetch=ON | 5.46776e+07 | options/s | std::erfc f64, single core, batch=496 |
| blackscholes | speedup_vs_scalar | AVX2/prefetch=ON | 12.5951 | x | same batch |
| bridge | graph_bytes | AVX2/prefetch=ON | 144448 | bytes | sizeof(CSRGraph) |
| bridge | graph_bytes | AVX2/prefetch=OFF | 144448 | bytes | sizeof(CSRGraph) |
| clob | fill_throughput_fps | AVX2/prefetch=ON | 2.48803e+06 | fills/s | includes refresh_liquidity re-quote per fill; pinned=1 |
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
| noise | p99_over_p50 | AVX2/prefetch=ON | 1 | ratio | 1.00 = interference does not reach p99 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=ON | 122.139 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=ON | 1.87058 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=ON | 4.72737e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=ON | 4.51519e+06 | events/s | unpaced; mutex+queue; pinned=1 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=OFF | 231.707 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=OFF | 0.222531 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=OFF | 4.73025e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=OFF | 4.55673e+06 | events/s | unpaced; mutex+queue; pinned=1 |

## 4.5 Cascade termination: why the shipped policy is the slow one

`run_cascade_tick()` takes a compile-time `CascadeTermination` policy.
`NoNewDefaults` stops at the first round producing no new default;
`RiskQuiescence` (the default) also keeps going while any risk score moves.
The obvious optimisation is unsafe, and `bench_convergence` is what shows it:

| Shock | NoNewDefaults | RiskQuiescence | Final state |
|---|---|---|---|
| -10% equities | 1 rnd / 0 def | 3 rnd / 0 def | diverged (risk, NAV) |
| -30% equities | 1 rnd / 0 def | 2 rnd / 0 def | identical |
| -50% equities | 1 rnd / 0 def | 2 rnd / 0 def | identical |
| **-80% equities** | **1 rnd / 0 def** | **1024 rnd / 22 def** | **diverged: all 22 defaults lost** |
| -80% crypto | 1 rnd / 0 def | 2 rnd / 0 def | identical |
| Lehman | 1 rnd / 0 def | 2 rnd / 0 def | identical |
| Covid | 1 rnd / 0 def | 1024 rnd / 0 def | diverged (risk) |
| **-50% all classes** | **3 rnd / 212 def** | **1024 rnd / 215 def** | **diverged: 3 defaults lost** |
| -90% all classes | 2 rnd / 500 def | 2 rnd / 500 def | identical |

4 of 9 diverge. The default set is compared exactly; risk and NAV within
1e-4 and 1e-9 relative.

The reason is the gap structure. Defaults do not arrive in one wave:

| Shock | Defaults | Last default at | Longest quiet gap |
|---|---|---|---|
| -80% equities | 22 | round 952 | **469 rounds** |
| -50% all classes | 215 | round 32 | 15 rounds |
| -90% all classes | 500 | round 0 | 0 rounds |

Under -80% equities a firm defaults, 469 consecutive rounds pass with nothing
observable, then another defaults. Stress contagion raises neighbour risk
fractionally per round until a firm crosses `DEFAULT_THRESH`. Any
stop-when-quiet rule needs patience above 469 rounds on this graph, which
costs more than it saves.

Two consequences:

1. Those late defaults are not counterparty contagion. A 469-round gap with
   no intervening event means they come from the gamma-hedging loop grinding
   prices down, not from the network.
2. **The 1024-round cap truncates real results.** The last default at round
   952 of 1024 means a higher cap would likely find more, so default counts
   for severe shocks are lower bounds.

Both are model problems, not loop problems, and are not fixed here.

## 5. Reproducing

```sh
./bench/run_all.sh          # build both variants, run everything, regenerate this file
./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness
```

Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, `bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.

