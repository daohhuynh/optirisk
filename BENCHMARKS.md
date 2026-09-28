# OptiRisk — Measured Benchmarks

Generated 2026-09-28 08:34:08 UTC by `bench/run_all.sh` + `bench/make_report.py`.

Every number below is a measurement. Nothing here is estimated, extrapolated,
rounded up, or carried over from another machine. Where a measurement could not
be taken, the row says so rather than being omitted.


## 1. Hardware and toolchain

```
OptiRisk benchmark environment
generated: 2026-09-28T08:31:16Z
git commit: 6cd1b20
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
| Ring handoff latency, p50 | 42 ns | 5,593 ns (mutex+queue) | 133.17x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99 | 72 ns | 17,946 ns (mutex+queue) | 249.25x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99.9 | 1,825 ns | 20,000 ns (mutex+queue) | 10.96x | SPSC 1024-slot, paced to queue depth ~1 |
| Transport throughput | 47,313,500 events/s | 4,481,140 events/s (mutex+queue) | 10.56x | producer unpaced (saturated) |
| Full pipeline, p50 | 10,702 ns | — | — | 500 nodes/7500 edges; -30% equities on node 0; state restored between  |
| Full pipeline, p99 | 23,406 ns | — | — | same run |
| Cascade tick (real 500n/7500e), p50 | 7,033 ns | — | — | real 500 nodes/7500 edges; -30% equities on node 0; state restored per |
| Cascade tick (real 500n/7500e), p99 | 7,834 ns | — | — | same run |
| Prefetch ablation (cascade p50) | 7,033 ns (ON) | 7,003 ns (OFF) | 1.00x | same source, two builds |
| SIMD phases only, p50 | 1,783 ns | 2,113 ns (prefetch OFF) | 1.19x | apply_shock_simd, 500 nodes |
| Option kernel throughput | 688.15 M options/s | 54.73 M options/s (std::erfc f64) | 12.57x | single core, batch=496, kernel path=AVX2-8lane-f32 |
| Delta max abs error | 7.406e-07 | double-precision reference | — | full grid, finite results only |
| Delta mean abs error | 1.394e-07 | double-precision reference | — | full grid |
| Fast-log max abs error | 1.176e-07 | double-precision reference | — | S/K in [0.70, 1.50] |
| rcp+NR max rel error | 1.645e-07 | double-precision reference | — | p in [1, 2000] |
| CLOB fill latency, p50 | 40 ns | — | — | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fi |
| CLOB fill latency, p99 | 80 ns | — | — | same run |
| CLOB fill throughput | 2,473,640 fills/s | — | — | includes refresh_liquidity re-quote per fill; pinned=1 |
| BBO publish -> observed, p50 | 42 ns | — | — | compute stamps then flip_buffers(); reader spins on active_buffer_idx  |
| BBO publish -> observed, p99 | 82 ns | — | — | same run |
| Python->C++ graph load (C++ side) | 23,655 ns | 2,885 ns (in-process memcpy floor) | 8.20x SLOWER | fopen + 21 fread, warm page cache |

### 3.1 `O(levels consumed)` — cost vs levels consumed

| Levels consumed | p50 (ns) | p99 (ns) | ns per level |
|---|---|---|---|
| 1 | 10 | 20 | 10.00 |
| 2 | 10 | 20 | 5.00 |
| 4 | 10 | 20 | 2.50 |
| 8 | 20 | 29 | 2.50 |
| 16 | 20 | 40 | 1.25 |
| 32 | 49 | 60 | 1.53 |
| 64 | 130 | 140 | 2.03 |
| 128 | 230 | 240 | 1.80 |
| 256 | 410 | 431 | 1.60 |

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
| N=100 E=1504 | 734,521 | 1,506,034 | 1,851,819 | 1,971,376 |
| N=200 E=3004 | 1,312,187 | 1,383,572 | 1,839,145 | 2,618,222 |
| N=300 E=4483 | 1,959,894 | 2,030,688 | 2,269,130 | 2,321,900 |
| N=400 E=5994 | 2,555,884 | 2,637,889 | 2,897,431 | 3,624,559 |
| N=500 E=7475 | 3,139,089 | 3,224,881 | 3,487,630 | 4,219,307 |
| N=100 E=1504 | 654,660 | 707,951 | 827,578 | 1,051,112 |
| N=200 E=3004 | 1,150,651 | 1,223,378 | 1,327,215 | 2,113,966 |
| N=300 E=4483 | 1,698,429 | 1,799,029 | 2,018,125 | 2,296,272 |
| N=400 E=5994 | 2,230,267 | 2,307,673 | 2,546,827 | 3,204,974 |
| N=500 E=7475 | 2,755,803 | 2,853,508 | 3,328,879 | 3,805,131 |

### 3.4 Branchless claim — disassembly

`bench/results/blackscholes_disasm.txt`: **12 conditional branches in probe_black_scholes**

A nonzero count means the hot loop is not branchless even where the arithmetic inside it is.


### 3.5 Python-side handoff

| Mechanism | p50 (ns) | p99 (ns) | bytes |
|---|---|---|---|
| tobytes_file_write | 348,444 | 486,082 | 143820 |
| pickle_write | 356,990 | 520,758 | 144737 |
| pickle_read | 65,613 | 92,334 | — |
| shared_memory_write | 28,233 | 48,741 | 143808 |

## 4. Full percentile output

Complete per-metric percentiles, exactly as emitted:

| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cascade | N=100 E=1504 | AVX2/prefetch=ON | 5000 | 747329.0 | 724,092 | 734,521 | 741,405 | 1,506,034 | 1,851,819 | 1,971,376 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=ON | 5000 | 1317977.6 | 1,305,905 | 1,312,187 | 1,323,488 | 1,383,572 | 1,839,145 | 2,618,222 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=ON | 5000 | 1964628.6 | 1,945,868 | 1,959,894 | 1,976,055 | 2,030,688 | 2,269,130 | 2,321,900 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=ON | 5000 | 2560336.5 | 2,535,986 | 2,555,884 | 2,574,379 | 2,637,889 | 2,897,431 | 3,624,559 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=ON | 5000 | 3145422.2 | 3,126,866 | 3,139,089 | 3,163,335 | 3,224,881 | 3,487,630 | 4,219,307 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=ON | 5000 | 7112.5 | 6,933 | 7,033 | 7,093 | 7,834 | 16,350 | 27,271 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=ON | 5000 | 1800.3 | 1,723 | 1,783 | 1,803 | 1,873 | 9,758 | 18,204 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=ON | 500 | 24040.1 | 23,504 | 23,655 | 23,855 | 35,016 | 39,304 | 39,304 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=ON | 500 | 3048.1 | 2,845 | 2,885 | 2,905 | 3,085 | 63,209 | 63,209 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| cascade | N=100 E=1504 | AVX2/prefetch=OFF | 5000 | 657129.9 | 637,468 | 654,660 | 670,661 | 707,951 | 827,578 | 1,051,112 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=OFF | 5000 | 1157404.9 | 1,139,880 | 1,150,651 | 1,179,776 | 1,223,378 | 1,327,215 | 2,113,966 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=OFF | 5000 | 1705822.8 | 1,670,205 | 1,698,429 | 1,737,703 | 1,799,029 | 2,018,125 | 2,296,272 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=OFF | 5000 | 2232587.3 | 2,186,394 | 2,230,267 | 2,267,207 | 2,307,673 | 2,546,827 | 3,204,974 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=OFF | 5000 | 2758584.6 | 2,710,256 | 2,755,803 | 2,788,204 | 2,853,508 | 3,328,879 | 3,805,131 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=OFF | 5000 | 7083.3 | 6,882 | 7,003 | 7,063 | 7,373 | 16,681 | 35,056 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=OFF | 5000 | 2134.9 | 2,073 | 2,113 | 2,133 | 2,193 | 10,519 | 14,547 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=OFF | 500 | 24257.1 | 23,594 | 23,755 | 23,915 | 37,190 | 51,878 | 51,878 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=OFF | 500 | 3092.6 | 2,895 | 2,935 | 2,965 | 3,065 | 64,402 | 64,402 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| clob | fill_levels_1 | AVX2/prefetch=ON | 20000 | 11.4 | 9 | 10 | 20 | 20 | 20 | 49 | depth=256, levels consumed=1 (observed 1), no BBO recording; pinned=1 |
| clob | fill_levels_2 | AVX2/prefetch=ON | 20000 | 11.4 | 9 | 10 | 20 | 20 | 20 | 170 | depth=256, levels consumed=2 (observed 2), no BBO recording; pinned=1 |
| clob | fill_levels_4 | AVX2/prefetch=ON | 20000 | 11.8 | 9 | 10 | 20 | 20 | 29 | 89 | depth=256, levels consumed=4 (observed 4), no BBO recording; pinned=1 |
| clob | fill_levels_8 | AVX2/prefetch=ON | 20000 | 16.5 | 9 | 20 | 20 | 29 | 40 | 10,119 | depth=256, levels consumed=8 (observed 8), no BBO recording; pinned=1 |
| clob | fill_levels_16 | AVX2/prefetch=ON | 20000 | 24.2 | 20 | 20 | 30 | 40 | 50 | 190 | depth=256, levels consumed=16 (observed 16), no BBO recording; pinned=1 |
| clob | fill_levels_32 | AVX2/prefetch=ON | 20000 | 46.2 | 29 | 49 | 50 | 60 | 89 | 13,405 | depth=256, levels consumed=32 (observed 32), no BBO recording; pinned=1 |
| clob | fill_levels_64 | AVX2/prefetch=ON | 20000 | 131.2 | 110 | 130 | 130 | 140 | 200 | 15,188 | depth=256, levels consumed=64 (observed 64), no BBO recording; pinned=1 |
| clob | fill_levels_128 | AVX2/prefetch=ON | 20000 | 234.9 | 220 | 230 | 240 | 240 | 310 | 12,654 | depth=256, levels consumed=128 (observed 128), no BBO recording; pinned=1 |
| clob | fill_levels_256 | AVX2/prefetch=ON | 20000 | 420.3 | 400 | 410 | 420 | 431 | 491 | 12,183 | depth=256, levels consumed=256 (observed 256), no BBO recording; pinned=1 |
| clob | fill_depth_8 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 89 | book depth=8, 1 level consumed; pinned=1 |
| clob | fill_depth_16 | AVX2/prefetch=ON | 20000 | 12.3 | 9 | 10 | 20 | 20 | 20 | 29 | book depth=16, 1 level consumed; pinned=1 |
| clob | fill_depth_32 | AVX2/prefetch=ON | 20000 | 13.5 | 9 | 10 | 20 | 20 | 20 | 10,700 | book depth=32, 1 level consumed; pinned=1 |
| clob | fill_depth_64 | AVX2/prefetch=ON | 20000 | 12.4 | 9 | 10 | 20 | 20 | 20 | 30 | book depth=64, 1 level consumed; pinned=1 |
| clob | fill_depth_128 | AVX2/prefetch=ON | 20000 | 12.4 | 9 | 10 | 20 | 20 | 20 | 30 | book depth=128, 1 level consumed; pinned=1 |
| clob | fill_depth_256 | AVX2/prefetch=ON | 20000 | 12.4 | 9 | 10 | 20 | 20 | 20 | 80 | book depth=256, 1 level consumed; pinned=1 |
| clob | fill_latency_mixed | AVX2/prefetch=ON | 200000 | 40.4 | 9 | 40 | 60 | 80 | 100 | 10,770 | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=1 |
| clob | bbo_publish_latency | AVX2/prefetch=ON | 100000 | 50.9 | 12 | 42 | 62 | 82 | 1,785 | 14,760 | compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=1 |
| noise | fixed_work_unit | AVX2/prefetch=ON | 2000000 | 309.1 | 210 | 310 | 310 | 310 | 431 | 32,802 | identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=ON | 2500000 | 56.2 | 12 | 42 | 62 | 72 | 1,825 | 108,939 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=ON | 2500000 | 7218.0 | 42 | 5,593 | 14,820 | 17,946 | 20,000 | 396,995 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_end_to_end | AVX2/prefetch=ON | 20000 | 10999.0 | 10,512 | 10,702 | 11,053 | 23,406 | 26,071 | 29,277 | 500 nodes/7500 edges; -30% equities on node 0; state restored between events (untimed); paced 300us; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=OFF | 2500000 | 53.4 | 12 | 42 | 62 | 72 | 1,805 | 105,331 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=OFF | 2500000 | 7151.6 | 42 | 5,372 | 14,850 | 18,016 | 19,819 | 115,080 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_end_to_end | AVX2/prefetch=OFF | 20000 | 11013.3 | 10,522 | 10,762 | 11,003 | 23,356 | 26,051 | 42,993 | 500 nodes/7500 edges; -30% equities on node 0; state restored between events (untimed); paced 300us; pinned=1 |

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
| blackscholes | kernel_throughput_ops | AVX2/prefetch=ON | 6.88151e+08 | options/s | single core, batch=496, kernel path=AVX2-8lane-f32 |
| blackscholes | scalar_throughput_ops | AVX2/prefetch=ON | 5.47263e+07 | options/s | std::erfc f64, single core, batch=496 |
| blackscholes | speedup_vs_scalar | AVX2/prefetch=ON | 12.5744 | x | same batch |
| bridge | graph_bytes | AVX2/prefetch=ON | 144448 | bytes | sizeof(CSRGraph) |
| bridge | graph_bytes | AVX2/prefetch=OFF | 144448 | bytes | sizeof(CSRGraph) |
| clob | fill_throughput_fps | AVX2/prefetch=ON | 2.47364e+06 | fills/s | includes refresh_liquidity re-quote per fill; pinned=1 |
| noise | p99_over_p50 | AVX2/prefetch=ON | 1 | ratio | 1.00 = interference does not reach p99 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=ON | 13.8889 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=ON | 0.671667 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=ON | 4.73135e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=ON | 4.48114e+06 | events/s | unpaced; mutex+queue; pinned=1 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=OFF | 0 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=OFF | 0.892558 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=OFF | 4.73044e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=OFF | 4.4979e+06 | events/s | unpaced; mutex+queue; pinned=1 |

## 5. Reproducing

```sh
./bench/run_all.sh          # build both variants, run everything, regenerate this file
./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness
```

Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, `bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.

