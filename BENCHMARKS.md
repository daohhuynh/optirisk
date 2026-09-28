# OptiRisk — Measured Benchmarks

Generated 2026-09-28 10:09:38 UTC by `bench/run_all.sh` + `bench/make_report.py`.

Every number below is a measurement. Nothing here is estimated, extrapolated,
rounded up, or carried over from another machine. Where a measurement could not
be taken, the row says so rather than being omitted.


## 1. Hardware and toolchain

```
OptiRisk benchmark environment
generated: 2026-09-28T10:05:43Z
git commit: bf5b065
git dirty: no

── Hardware ──
cpu: AMD EPYC 9V45 96-Core Processor
arch: x86_64
logical cores: 4
os: Ubuntu 24.04.5 LTS
kernel: 6.17.0-1022-azure
avx2: yes
avx512f: yes
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

**Timer resolution:** `effective timer granularity: 0 ns (smallest nonzero back-to-back delta)`

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
| Ring handoff latency, p50 | 50 ns | 3,055 ns (mutex+queue) | 61.10x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99 | 80 ns | 9,205 ns (mutex+queue) | 115.06x | SPSC 1024-slot, paced to queue depth ~1 |
| Ring handoff latency, p99.9 | 1,162 ns | 26,874 ns (mutex+queue) | 23.13x | SPSC 1024-slot, paced to queue depth ~1 |
| Transport throughput | 41,581,500 events/s | 6,843,040 events/s (mutex+queue) | 6.08x | producer unpaced (saturated) |
| Cascade tick (real 500n/7500e), p50 | 4,156 ns | — | — | real 500 nodes/7500 edges; -30% equities on node 0; state restored per |
| Cascade tick (real 500n/7500e), p99 | 4,326 ns | — | — | same run |
| Prefetch ablation (cascade p50) | 4,156 ns (ON) | 3,946 ns (OFF) | 0.95x | same source, two builds |
| SIMD phases only, p50 | 1,141 ns | 1,141 ns (prefetch OFF) | 1.00x | apply_shock_simd, 500 nodes |
| Option kernel throughput | 1,053.14 M options/s | 81.48 M options/s (std::erfc f64) | 12.93x | single core, batch=496, kernel path=AVX2-8lane-f32 |
| Delta max abs error | 7.406e-07 | double-precision reference | — | full grid, finite results only |
| Delta mean abs error | 1.394e-07 | double-precision reference | — | full grid |
| Fast-log max abs error | 1.176e-07 | double-precision reference | — | S/K in [0.70, 1.50] |
| rcp+NR max rel error | 1.645e-07 | double-precision reference | — | p in [1, 2000] |
| CLOB fill latency, p50 | 20 ns | — | — | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fi |
| CLOB fill latency, p99 | 40 ns | — | — | same run |
| CLOB fill throughput | 4,568,930 fills/s | — | — | includes refresh_liquidity re-quote per fill; pinned=1 |
| BBO publish -> observed, p50 | 50 ns | — | — | compute stamps then flip_buffers(); reader spins on active_buffer_idx  |
| BBO publish -> observed, p99 | 80 ns | — | — | same run |
| Python->C++ graph load (C++ side) | 15,764 ns | 1,071 ns (in-process memcpy floor) | 14.72x SLOWER | fopen + 21 fread, warm page cache |

### 3.1 `O(levels consumed)` — cost vs levels consumed

| Levels consumed | p50 (ns) | p99 (ns) | ns per level |
|---|---|---|---|
| 1 | 10 | 20 | 10.00 |
| 2 | 10 | 20 | 5.00 |
| 4 | 10 | 20 | 2.50 |
| 8 | 10 | 30 | 1.25 |
| 16 | 10 | 20 | 0.62 |
| 32 | 30 | 30 | 0.94 |
| 64 | 70 | 80 | 1.09 |
| 128 | 160 | 180 | 1.25 |
| 256 | 350 | 380 | 1.37 |

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
| N=100 E=1504 | 375,735 | 386,983 | 427,805 | 427,805 |
| N=200 E=3004 | 669,486 | 717,469 | 934,993 | 934,993 |
| N=300 E=4483 | 991,219 | 1,036,789 | 1,079,094 | 1,079,094 |
| N=400 E=5994 | 1,319,732 | 1,364,461 | 1,398,714 | 1,398,714 |
| N=500 E=7475 | 1,657,390 | 1,733,827 | 2,066,066 | 2,066,066 |
| N=100 E=1504 | 368,654 | 400,082 | 417,038 | 417,038 |
| N=200 E=3004 | 631,596 | 684,467 | 791,221 | 791,221 |
| N=300 E=4483 | 970,255 | 1,566,578 | 2,436,189 | 2,436,189 |
| N=400 E=5994 | 1,299,138 | 1,371,629 | 1,425,191 | 1,425,191 |
| N=500 E=7475 | 1,608,652 | 1,756,629 | 2,035,786 | 2,035,786 |

### 3.4 Branchless claim — disassembly

`bench/results/blackscholes_disasm.txt`: **12 conditional branches in probe_black_scholes**

A nonzero count means the hot loop is not branchless even where the arithmetic inside it is.


### 3.5 Python-side handoff

| Mechanism | p50 (ns) | p99 (ns) | bytes |
|---|---|---|---|
| tobytes_file_write | 368,087 | 3,459,994 | 143820 |
| pickle_write | 352,854 | 3,871,828 | 144737 |
| pickle_read | 28,643 | 38,989 | — |
| shared_memory_write | 14,643 | 20,891 | 143808 |

## 4.6 Where the option kernel speedup comes from

The headline figure compares scalar f64 libm against AVX2 f32, which changes
two things at once. The middle rung is scalar f32 using the kernel's exact
approximations, so A to B isolates math and B to C isolates vectorization.
All three run back to back on one thread over the same batch.

| Variant | M options/s |
|---|---|
| A. scalar f64, libm `std::erfc` | 81.5 |
| B. scalar f32, same approximations | 123.9 |
| C. AVX2 f32, 8 lanes (shipped) | 1,053.1 |

- Cheaper math (A to B): **1.52x**
- Vectorization (B to C): **8.50x**
- Total: **12.93x**

B and C agree to 0.000e+00 max absolute difference, so the
split compares two implementations of one function.


## 4.7 BBO: in-process visibility is not the wire

| Stage | p50 | p99 | Transport |
|---|---|---|---|
| Flip to in-process reader observes | 50 ns | 80 ns | Two threads, one process, shared ping-pong buffer. No socket. |
| `broadcast_bbo` sendmsg | 5,398 ns | 7,601 ns | `sendmsg` to 239.255.0.1:9090, 2-entry iovec |

Fill-to-wire is the sum, and the syscall dominates it. UDP multicast appears
in exactly one place in the project: `UdpPublisher`
(`backend/src/network/udp_publisher.hpp`), constructed in `main.cpp` and called
from the broadcast thread. A browser cannot receive multicast, so the frontend
uses the WebSocket path and the UDP feed has no consumer in this repository.


## 4.5 Cascade termination

Stopping at the first round with no new default looks free. It is not; this
is why `CascadeTermination::RiskQuiescence` remains the default.

```
═══════════════════════════════════════════════════════════════════════
  bench_convergence — new termination vs legacy, same final state?
  build: AVX2 | prefetch=ON
  tick rate: 0.385243 ns/tick (2595.765 MHz counter)
  effective timer granularity: 0 ns (smallest nonzero back-to-back delta)
═══════════════════════════════════════════════════════════════════════
  thread pinning: ACTIVE
  graph: 500 nodes, 7500 edges
  risk tolerance 1e-04, NAV relative tolerance 1e-09; default set compared exactly

  shock                    NoNewDefaults          RiskQuiescence (shipped) final state
                           rounds / def / liq     rounds / def / liq     
  ------------------------------------------------------------------------------------------------
  -10% equities            1 / 0 / 0              3 / 0 / 0              *** DIVERGED ***
      default-set mismatches : 0 node(s)
      risk    mismatches     : 499 node(s), max delta 1.821e-03
      nav     mismatches     : 500 node(s), max rel delta 1.824e-03
      counters               : defaults 0 vs 0, liquidations 0 vs 0
  -30% equities            1 / 0 / 0              2 / 0 / 0              IDENTICAL
  -50% equities            1 / 0 / 0              2 / 0 / 0              IDENTICAL
  -80% equities            1 / 0 / 0              1024 / 22 / 22         *** DIVERGED ***
      last default at round 952; longest quiet gap 469 rounds -> any stop-when-quiet rule needs patience > 469
      default-set mismatches : 22 node(s)
      risk    mismatches     : 34 node(s), max delta 8.458e-01
      nav     mismatches     : 22 node(s), max rel delta 2.793e+01
      counters               : defaults 0 vs 22, liquidations 0 vs 22
  -80% crypto              1 / 0 / 0              2 / 0 / 0              IDENTICAL
  Lehman (-40/-25/-15)     1 / 0 / 0              2 / 0 / 0              IDENTICAL
  Covid (-35/-10/-50)      1 / 0 / 0              1024 / 0 / 0           *** DIVERGED ***
      default-set mismatches : 0 node(s)
      risk    mismatches     : 15 node(s), max delta 8.283e-02
      nav     mismatches     : 0 node(s), max rel delta 0.000e+00
      counters               : defaults 0 vs 0, liquidations 0 vs 0
  -50% all classes         3 / 212 / 212          1024 / 215 / 215       *** DIVERGED ***
      last default at round 32; longest quiet gap 15 rounds -> any stop-when-quiet rule needs patience > 15
      default-set mismatches : 3 node(s)
      risk    mismatches     : 4 node(s), max delta 4.098e-01
      nav     mismatches     : 3 node(s), max rel delta 1.067e+01
      counters               : defaults 212 vs 215, liquidations 212 vs 215
  -90% all classes         2 / 500 / 500          2 / 500 / 500          IDENTICAL
      last default at round 0; longest quiet gap 0 rounds -> any stop-when-quiet rule needs patience > 0

  ------------------------------------------------------------------------------------------------
  RESULT: 4 of 9 shocks DIVERGED. NoNewDefaults is NOT a safe
          default: defaults in this model arrive in bursts separated
          by long quiet stretches, so the first quiet round is not
          the end of the cascade. RiskQuiescence stays the default.
```

## 4.8 Contagion channel ablation

Two compile-time flags remove one propagation path each, with no logic change.
Four builds cover the 2x2. The both-off corner supplies the baseline: firms that
fail on the direct mark-to-market alone belong to neither channel, and without
that denominator any per-channel share would misattribute them. Round caps are
swept until the default count stops changing.

```
====================================================================================================
  CONTAGION CHANNEL ABLATION
====================================================================================================

  TOTAL DEFAULTS BY VARIANT (out of 500 nodes)

  scenario                          full          no-gamma   no-counterparty           neither
  --------------------------------------------------------------------------------------------
  -10% equities                        0                 0                 0                 0
  -30% equities                        0                 0                 0                 0
  -50% equities                        0                 0                 0                 0
  -80% equities                       30                30                 0                 0
  -80% crypto                          0                 0                 0                 0
  Lehman                               0                 0                 0                 0
  Covid                                0                 0                 0                 0
  -50% all classes                   216               216               189               189
  -90% all classes                   500               500               500               500

  ROUND OF LAST DEFAULT (-1 = no defaults)

  scenario                          full          no-gamma   no-counterparty           neither
  --------------------------------------------------------------------------------------------
  -10% equities                       -1                -1                -1                -1
  -30% equities                       -1                -1                -1                -1
  -50% equities                       -1                -1                -1                -1
  -80% equities                     4143              4143                -1                -1
  -80% crypto                         -1                -1                -1                -1
  Lehman                              -1                -1                -1                -1
  Covid                               -1                -1                -1                -1
  -50% all classes                  1434              1434                 0                 0
  -90% all classes                     0                 0                 0                 0

  DECOMPOSITION: where each default comes from

  scenario               full  baseline   gamma  counterp  interact   additive?   
  ----------------------------------------------------------------------------------------
  -10% equities             0         0       0         0         0   additive    
  -30% equities             0         0       0         0         0   additive    
  -50% equities             0         0       0         0         0   additive    
  -80% equities            30         0       0        30         0   additive    
  -80% crypto               0         0       0         0         0   additive    
  Lehman                    0         0       0         0         0   additive    
  Covid                     0         0       0         0         0   additive    
  -50% all classes        216       189       0        27         0   additive    
  -90% all classes        500       500       0         0         0   additive    

  SHARE OF DEFAULTS BY CHANNEL

  scenario              baseline    gamma  counterp  interact
  ------------------------------------------------------------
  -10% equities                                 (no defaults)
  -30% equities                                 (no defaults)
  -50% equities                                 (no defaults)
  -80% equities             0.0%     0.0%    100.0%      0.0%
  -80% crypto                                   (no defaults)
  Lehman                                        (no defaults)
  Covid                                         (no defaults)
  -50% all classes         87.5%     0.0%     12.5%      0.0%
  -90% all classes        100.0%     0.0%      0.0%      0.0%

  1024-ROUND CAP: was it truncating?

    full               TRUNCATED: -80% equities, -50% all classes
    no-gamma           TRUNCATED: -80% equities, -50% all classes
    no-counterparty    adequate for every scenario
    neither            adequate for every scenario

  Channels are additive across every scenario; shares are meaningful.
```

## 4.9 Full percentile output

Complete per-metric percentiles, exactly as emitted:

| Bench | Metric | Build | n | mean | min | p50 | p90 | p99 | p99.9 | max | Conditions |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cascade | N=100 E=1504 | AVX2/prefetch=ON | 400 | 376770.5 | 370,167 | 375,735 | 381,184 | 386,983 | 427,805 | 427,805 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=ON | 400 | 671186.6 | 647,622 | 669,486 | 679,702 | 717,469 | 934,993 | 934,993 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=ON | 400 | 993262.3 | 978,810 | 991,219 | 999,842 | 1,036,789 | 1,079,094 | 1,079,094 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=ON | 400 | 1322039.6 | 1,307,884 | 1,319,732 | 1,331,591 | 1,364,461 | 1,398,714 | 1,398,714 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=ON | 400 | 1660716.3 | 1,634,044 | 1,657,390 | 1,668,617 | 1,733,827 | 2,066,066 | 2,066,066 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=ON | 5000 | 4199.0 | 4,026 | 4,156 | 4,216 | 4,326 | 9,805 | 10,315 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=ON | 5000 | 1150.0 | 1,111 | 1,141 | 1,151 | 1,171 | 1,942 | 11,497 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=ON | 500 | 15888.0 | 15,613 | 15,764 | 15,864 | 21,362 | 25,298 | 25,298 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=ON | 500 | 1158.3 | 1,061 | 1,071 | 1,071 | 1,081 | 44,908 | 44,908 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| cascade | N=100 E=1504 | AVX2/prefetch=OFF | 400 | 370301.3 | 356,425 | 368,654 | 388,614 | 400,082 | 417,038 | 417,038 | synthetic N=100 E=1504; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=200 E=3004 | AVX2/prefetch=OFF | 400 | 638282.4 | 619,648 | 631,596 | 662,564 | 684,467 | 791,221 | 791,221 | synthetic N=200 E=3004; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=300 E=4483 | AVX2/prefetch=OFF | 400 | 993468.6 | 951,967 | 970,255 | 1,013,131 | 1,566,578 | 2,436,189 | 2,436,189 | synthetic N=300 E=4483; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=400 E=5994 | AVX2/prefetch=OFF | 400 | 1308630.5 | 1,257,575 | 1,299,138 | 1,352,670 | 1,371,629 | 1,425,191 | 1,425,191 | synthetic N=400 E=5994; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | N=500 E=7475 | AVX2/prefetch=OFF | 400 | 1622046.3 | 1,585,397 | 1,608,652 | 1,659,230 | 1,756,629 | 2,035,786 | 2,035,786 | synthetic N=500 E=7475; -30% equities; state restored per iter (untimed); pinned=1 |
| cascade | run_cascade_tick_real500 | AVX2/prefetch=OFF | 5000 | 3992.8 | 3,835 | 3,946 | 4,056 | 4,086 | 9,504 | 15,263 | real 500 nodes/7500 edges; -30% equities on node 0; state restored per iter (untimed); pinned=1 |
| cascade | apply_shock_simd_real500 | AVX2/prefetch=OFF | 5000 | 1146.5 | 1,091 | 1,141 | 1,151 | 1,161 | 3,615 | 11,537 | apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real 500 nodes; pinned=1 |
| bridge | cpp_fread_load | AVX2/prefetch=OFF | 500 | 15997.3 | 15,734 | 15,874 | 15,944 | 21,522 | 26,130 | 26,130 | fopen + 21 fread of optirisk_memory.bin into .bss; warm page cache; pinned=1 |
| bridge | memcpy_floor | AVX2/prefetch=OFF | 500 | 1193.1 | 1,071 | 1,081 | 1,081 | 1,091 | 47,282 | 47,282 | in-process memcpy of sizeof(CSRGraph)=144448 bytes; the floor a true zero-copy path would beat |
| clob | fill_levels_1 | AVX2/prefetch=ON | 20000 | 12.3 | 0 | 10 | 20 | 20 | 20 | 100 | depth=256, levels consumed=1 (observed 1), no BBO recording; pinned=1 |
| clob | fill_levels_2 | AVX2/prefetch=ON | 20000 | 12.3 | 0 | 10 | 20 | 20 | 20 | 100 | depth=256, levels consumed=2 (observed 2), no BBO recording; pinned=1 |
| clob | fill_levels_4 | AVX2/prefetch=ON | 20000 | 12.2 | 0 | 10 | 20 | 20 | 20 | 40 | depth=256, levels consumed=4 (observed 4), no BBO recording; pinned=1 |
| clob | fill_levels_8 | AVX2/prefetch=ON | 20000 | 11.0 | 0 | 10 | 20 | 30 | 30 | 100 | depth=256, levels consumed=8 (observed 8), no BBO recording; pinned=1 |
| clob | fill_levels_16 | AVX2/prefetch=ON | 20000 | 11.5 | 0 | 10 | 20 | 20 | 20 | 50 | depth=256, levels consumed=16 (observed 16), no BBO recording; pinned=1 |
| clob | fill_levels_32 | AVX2/prefetch=ON | 20000 | 25.7 | 20 | 30 | 30 | 30 | 40 | 9,194 | depth=256, levels consumed=32 (observed 32), no BBO recording; pinned=1 |
| clob | fill_levels_64 | AVX2/prefetch=ON | 20000 | 73.6 | 60 | 70 | 80 | 80 | 100 | 12,028 | depth=256, levels consumed=64 (observed 64), no BBO recording; pinned=1 |
| clob | fill_levels_128 | AVX2/prefetch=ON | 20000 | 167.7 | 150 | 160 | 180 | 180 | 200 | 13,861 | depth=256, levels consumed=128 (observed 128), no BBO recording; pinned=1 |
| clob | fill_levels_256 | AVX2/prefetch=ON | 20000 | 359.9 | 330 | 350 | 380 | 380 | 400 | 8,953 | depth=256, levels consumed=256 (observed 256), no BBO recording; pinned=1 |
| clob | fill_depth_8 | AVX2/prefetch=ON | 20000 | 12.6 | 0 | 10 | 20 | 20 | 20 | 30 | book depth=8, 1 level consumed; pinned=1 |
| clob | fill_depth_16 | AVX2/prefetch=ON | 20000 | 12.9 | 0 | 10 | 20 | 20 | 20 | 5,678 | book depth=16, 1 level consumed; pinned=1 |
| clob | fill_depth_32 | AVX2/prefetch=ON | 20000 | 12.8 | 0 | 10 | 20 | 20 | 20 | 30 | book depth=32, 1 level consumed; pinned=1 |
| clob | fill_depth_64 | AVX2/prefetch=ON | 20000 | 12.7 | 0 | 10 | 20 | 20 | 20 | 30 | book depth=64, 1 level consumed; pinned=1 |
| clob | fill_depth_128 | AVX2/prefetch=ON | 20000 | 12.9 | 0 | 10 | 20 | 20 | 20 | 30 | book depth=128, 1 level consumed; pinned=1 |
| clob | fill_depth_256 | AVX2/prefetch=ON | 20000 | 12.8 | 0 | 10 | 20 | 20 | 20 | 30 | book depth=256, 1 level consumed; pinned=1 |
| clob | fill_latency_mixed | AVX2/prefetch=ON | 200000 | 18.6 | 0 | 20 | 30 | 40 | 50 | 5,828 | 5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=1 |
| clob | bbo_publish_latency | AVX2/prefetch=ON | 100000 | 49.2 | 10 | 50 | 60 | 80 | 90 | 43,106 | compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=1 |
| clob | bbo_multicast_sendmsg | AVX2/prefetch=ON | 20000 | 5569.1 | 3,956 | 5,398 | 6,239 | 7,601 | 35,223 | 1,077,527 | sendmsg to 239.255.0.1:9090, 9 BboUpdate entries (144 bytes) via 2-entry iovec; pinned=1 |
| noise | fixed_work_unit | AVX2/prefetch=ON | 2000000 | 238.7 | 80 | 240 | 240 | 240 | 250 | 79,769 | identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=ON | 2500000 | 56.5 | 0 | 50 | 70 | 80 | 1,162 | 83,196 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=ON | 2500000 | 3710.3 | 30 | 3,055 | 6,972 | 9,205 | 26,874 | 1,003,899 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_no_defaults | AVX2/prefetch=ON | 400 | 6499.3 | 5,479 | 6,381 | 6,531 | 8,323 | 26,854 | 26,854 | no defaults: 500 nodes/7500 edges, 2 rounds, 0 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_small_cascade | AVX2/prefetch=ON | 400 | 2813743.2 | 2,651,820 | 2,812,533 | 2,897,161 | 2,946,521 | 2,964,841 | 2,964,841 | small cascade: 500 nodes/7500 edges, 1024 rounds, 22 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_large_cascade | AVX2/prefetch=ON | 400 | 4755121.8 | 4,600,282 | 4,716,652 | 4,876,663 | 4,957,394 | 5,351,808 | 5,351,808 | large cascade: 500 nodes/7500 edges, 1024 rounds, 215 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | disruptor_handoff_paced | AVX2/prefetch=OFF | 2500000 | 59.7 | 0 | 50 | 70 | 70 | 1,152 | 101,125 | SPSC 1024-slot ring; paced 2000ns (queue depth ~1); 5 runs pooled; pinned=1 |
| ring | mutex_handoff_paced | AVX2/prefetch=OFF | 2500000 | 3680.0 | 30 | 3,065 | 6,931 | 9,024 | 15,454 | 170,171 | std::mutex + std::queue + condition_variable, 1024-bounded; paced 2000ns; 5 runs pooled; pinned=1 |
| ring | pipeline_no_defaults | AVX2/prefetch=OFF | 400 | 6198.5 | 5,969 | 6,170 | 6,280 | 6,911 | 8,393 | 8,393 | no defaults: 500 nodes/7500 edges, 2 rounds, 0 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_small_cascade | AVX2/prefetch=OFF | 400 | 3030329.0 | 2,573,214 | 3,149,127 | 3,210,041 | 3,309,092 | 3,375,374 | 3,375,374 | small cascade: 500 nodes/7500 edges, 1024 rounds, 22 defaults; state restored between events (untimed); paced 20000us; pinned=1 |
| ring | pipeline_large_cascade | AVX2/prefetch=OFF | 400 | 4463353.5 | 3,608,541 | 4,451,393 | 4,699,123 | 4,942,266 | 5,101,370 | 5,101,370 | large cascade: 500 nodes/7500 edges, 1024 rounds, 215 defaults; state restored between events (untimed); paced 20000us; pinned=1 |

### Scalar metrics

| Bench | Metric | Build | Value | Unit | Conditions |
|---|---|---|---|---|---|
| ablation | full|-10% equities | AVX2/prefetch=ON | 0 | defaults | variant=full; defaults=0; last_default_round=none; rounds=4; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-10% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=full; defaults=0; last_default_round=none; rounds=4; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-30% equities | AVX2/prefetch=ON | 0 | defaults | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-30% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-50% equities | AVX2/prefetch=ON | 0 | defaults | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-50% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-80% equities | AVX2/prefetch=ON | 30 | defaults | variant=full; defaults=30; last_default_round=4143; rounds=16384; settled_cap=16384; defaults_at_1024=22; 1024 cap TRUNCATED |
| ablation | full|-80% equities|last_round | AVX2/prefetch=ON | 4143 | round | variant=full; defaults=30; last_default_round=4143; rounds=16384; settled_cap=16384; defaults_at_1024=22; 1024 cap TRUNCATED |
| ablation | full|-80% crypto | AVX2/prefetch=ON | 0 | defaults | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-80% crypto|last_round | AVX2/prefetch=ON | -1 | round | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|Lehman | AVX2/prefetch=ON | 0 | defaults | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|Lehman|last_round | AVX2/prefetch=ON | -1 | round | variant=full; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|Covid | AVX2/prefetch=ON | 0 | defaults | variant=full; defaults=0; last_default_round=none; rounds=2048; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|Covid|last_round | AVX2/prefetch=ON | -1 | round | variant=full; defaults=0; last_default_round=none; rounds=2048; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | full|-50% all classes | AVX2/prefetch=ON | 216 | defaults | variant=full; defaults=216; last_default_round=1434; rounds=1436; settled_cap=4096; defaults_at_1024=215; 1024 cap TRUNCATED |
| ablation | full|-50% all classes|last_round | AVX2/prefetch=ON | 1434 | round | variant=full; defaults=216; last_default_round=1434; rounds=1436; settled_cap=4096; defaults_at_1024=215; 1024 cap TRUNCATED |
| ablation | full|-90% all classes | AVX2/prefetch=ON | 500 | defaults | variant=full; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | full|-90% all classes|last_round | AVX2/prefetch=ON | 0 | round | variant=full; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | neither|-10% equities | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-10% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-30% equities | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-30% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-50% equities | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-50% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-80% equities | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-80% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-80% crypto | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-80% crypto|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|Lehman | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|Lehman|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|Covid | AVX2/prefetch=ON | 0 | defaults | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|Covid|last_round | AVX2/prefetch=ON | -1 | round | variant=neither; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | neither|-50% all classes | AVX2/prefetch=ON | 189 | defaults | variant=neither; defaults=189; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=189; 1024 cap adequate |
| ablation | neither|-50% all classes|last_round | AVX2/prefetch=ON | 0 | round | variant=neither; defaults=189; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=189; 1024 cap adequate |
| ablation | neither|-90% all classes | AVX2/prefetch=ON | 500 | defaults | variant=neither; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | neither|-90% all classes|last_round | AVX2/prefetch=ON | 0 | round | variant=neither; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | no-counterparty|-10% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=4; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-10% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=4; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-30% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-30% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-50% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-50% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-80% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-80% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-80% crypto | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-80% crypto|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|Lehman | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|Lehman|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|Covid | AVX2/prefetch=ON | 0 | defaults | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|Covid|last_round | AVX2/prefetch=ON | -1 | round | variant=no-counterparty; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-counterparty|-50% all classes | AVX2/prefetch=ON | 189 | defaults | variant=no-counterparty; defaults=189; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=189; 1024 cap adequate |
| ablation | no-counterparty|-50% all classes|last_round | AVX2/prefetch=ON | 0 | round | variant=no-counterparty; defaults=189; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=189; 1024 cap adequate |
| ablation | no-counterparty|-90% all classes | AVX2/prefetch=ON | 500 | defaults | variant=no-counterparty; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | no-counterparty|-90% all classes|last_round | AVX2/prefetch=ON | 0 | round | variant=no-counterparty; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | no-gamma|-10% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-10% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-30% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-30% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-50% equities | AVX2/prefetch=ON | 0 | defaults | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-50% equities|last_round | AVX2/prefetch=ON | -1 | round | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-80% equities | AVX2/prefetch=ON | 30 | defaults | variant=no-gamma; defaults=30; last_default_round=4143; rounds=16384; settled_cap=16384; defaults_at_1024=22; 1024 cap TRUNCATED |
| ablation | no-gamma|-80% equities|last_round | AVX2/prefetch=ON | 4143 | round | variant=no-gamma; defaults=30; last_default_round=4143; rounds=16384; settled_cap=16384; defaults_at_1024=22; 1024 cap TRUNCATED |
| ablation | no-gamma|-80% crypto | AVX2/prefetch=ON | 0 | defaults | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-80% crypto|last_round | AVX2/prefetch=ON | -1 | round | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|Lehman | AVX2/prefetch=ON | 0 | defaults | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|Lehman|last_round | AVX2/prefetch=ON | -1 | round | variant=no-gamma; defaults=0; last_default_round=none; rounds=2; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|Covid | AVX2/prefetch=ON | 0 | defaults | variant=no-gamma; defaults=0; last_default_round=none; rounds=2048; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|Covid|last_round | AVX2/prefetch=ON | -1 | round | variant=no-gamma; defaults=0; last_default_round=none; rounds=2048; settled_cap=2048; defaults_at_1024=0; 1024 cap adequate |
| ablation | no-gamma|-50% all classes | AVX2/prefetch=ON | 216 | defaults | variant=no-gamma; defaults=216; last_default_round=1434; rounds=1436; settled_cap=4096; defaults_at_1024=215; 1024 cap TRUNCATED |
| ablation | no-gamma|-50% all classes|last_round | AVX2/prefetch=ON | 1434 | round | variant=no-gamma; defaults=216; last_default_round=1434; rounds=1436; settled_cap=4096; defaults_at_1024=215; 1024 cap TRUNCATED |
| ablation | no-gamma|-90% all classes | AVX2/prefetch=ON | 500 | defaults | variant=no-gamma; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| ablation | no-gamma|-90% all classes|last_round | AVX2/prefetch=ON | 0 | round | variant=no-gamma; defaults=500; last_default_round=0; rounds=2; settled_cap=2048; defaults_at_1024=500; 1024 cap adequate |
| blackscholes | delta_nonfinite_count | AVX2/prefetch=ON | 0 | count | inputs where the kernel returned NaN/Inf instead of a delta |
| blackscholes | delta_max_abs_err | AVX2/prefetch=ON | 7.40648e-07 | abs | full grid, finite results only |
| blackscholes | delta_mean_abs_err | AVX2/prefetch=ON | 1.39365e-07 | abs | full grid |
| blackscholes | delta_max_abs_err_atm | AVX2/prefetch=ON | 7.07965e-07 | abs | |ln(S/K)|<=0.05 |
| blackscholes | delta_max_abs_err_far | AVX2/prefetch=ON | 7.40648e-07 | abs | |ln(S/K)|>0.20 |
| blackscholes | unpriced_tail_options | AVX2/prefetch=ON | 0 | count | count=500; options the kernel never wrote |
| blackscholes | fastlog_max_abs_err | AVX2/prefetch=ON | 1.17632e-07 | abs | S/K in [0.70, 1.50] |
| blackscholes | rcp_nr_max_rel_err | AVX2/prefetch=ON | 1.64524e-07 | relative | p in [1, 2000] |
| blackscholes | f32_scalar_vs_avx2_max_diff | AVX2/prefetch=ON | 0 | abs | scalar f32 approx vs AVX2 kernel, same inputs |
| blackscholes | f32_scalar_throughput_ops | AVX2/prefetch=ON | 1.23949e+08 | options/s | scalar f32, same approximations as the AVX2 kernel, single lane |
| blackscholes | speedup_math_only | AVX2/prefetch=ON | 1.52129 | x | f64 libm -> f32 approx, both scalar |
| blackscholes | speedup_vectorization_only | AVX2/prefetch=ON | 8.49655 | x | f32 approx scalar -> AVX2 8-lane |
| blackscholes | kernel_throughput_ops | AVX2/prefetch=ON | 1.05314e+09 | options/s | single core, batch=496, kernel path=AVX2-8lane-f32 |
| blackscholes | scalar_throughput_ops | AVX2/prefetch=ON | 8.1476e+07 | options/s | std::erfc f64, single core, batch=496 |
| blackscholes | speedup_vs_scalar | AVX2/prefetch=ON | 12.9257 | x | same batch |
| bridge | graph_bytes | AVX2/prefetch=ON | 144448 | bytes | sizeof(CSRGraph) |
| bridge | graph_bytes | AVX2/prefetch=OFF | 144448 | bytes | sizeof(CSRGraph) |
| clob | fill_throughput_fps | AVX2/prefetch=ON | 4.56893e+06 | fills/s | includes refresh_liquidity re-quote per fill; pinned=1 |
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
| ring | disruptor_p99_spread_pct | AVX2/prefetch=ON | 14.2857 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=ON | 64.9036 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=ON | 4.15815e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=ON | 6.84304e+06 | events/s | unpaced; mutex+queue; pinned=1 |
| ring | disruptor_p99_spread_pct | AVX2/prefetch=OFF | 14.2857 | percent | p99 spread across 5 runs |
| ring | mutex_p99_spread_pct | AVX2/prefetch=OFF | 1.00402 | percent | p99 spread across 5 runs |
| ring | disruptor_throughput_eps | AVX2/prefetch=OFF | 4.32643e+07 | events/s | unpaced; SPSC ring; pinned=1 |
| ring | mutex_throughput_eps | AVX2/prefetch=OFF | 6.96488e+06 | events/s | unpaced; mutex+queue; pinned=1 |

## 5. Reproducing

```sh
./bench/run_all.sh          # build both variants, run everything, regenerate this file
./bench/run_all.sh --quick  # smaller iteration counts, for smoke-testing the harness
```

Raw evidence lands in `bench/results/`: `environment.txt`, `all_results.csv`, `bench_*.log`, per-sample `raw_*.csv`, and `blackscholes_disasm.txt`.

