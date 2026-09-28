# OptiRisk

**Real-time counterparty risk simulation at HFT speeds.**

OptiRisk is a high-performance counterparty credit-risk engine that models cascading default propagation across a 500-node financial network. Architected identically to a Tier-1 trading system, it features a bare-metal C++23 backend and a strict binary wire contract to a Next.js/WebGL frontend. 

The system operates with a strict zero-allocation hot path built on LMAX Disruptor ring
buffers, AVX2 vectorization, and cache-optimal Struct-of-Arrays (SoA) layouts. Every
performance figure below is measured by the suite in `bench/`; see **[BENCHMARKS.md](BENCHMARKS.md)**
for the hardware, methodology, and raw per-sample distributions.

## The Critical Path

Every figure in this section comes from a single run on one machine: an AMD EPYC 9V45,
AVX2, GCC 13.3, Ubuntu 24.04, threads pinned via `pthread_setaffinity_np`. Mixing numbers
from different runs or different CPUs would make the ratios meaningless, so the suite
records the machine alongside every result, and **[BENCHMARKS.md](BENCHMARKS.md) is
regenerated from the same run** rather than maintained by hand. GitHub-hosted runners vary
between jobs, so absolute latencies here will not reproduce exactly on a different runner;
the ratios are the durable part.

That machine's own noise floor was characterised first, by timing byte-identical work:
inflation over p50 is 1.00x at p99 and 1.39x at p99.9. So **p99 here is signal and p99.9
is not**, and no p99.9 is quoted below. Ingress and egress remain **engineering
estimates**: they are not instrumented, and are labelled as such.

| Stage | Measured | Notes |
|---|---|---|
| Disruptor ring handoff | **50 ns p50 / 80 ns p99** | SPSC, paced to queue depth ~1; p99 reproduced across 5 independent runs |
| Full pipeline, no defaults | **6.4 µs p50 / 8.3 µs p99** | 500 nodes / 7500 edges, -30% equities, settles in 2 rounds |
| Full pipeline, small cascade | **2.81 ms p50** | -80% equities, 30 defaults |
| Full pipeline, large cascade | **4.72 ms p50** | -50% across all five classes, 216 defaults |
| `run_cascade_tick` alone | **4.16 µs p50** | -30% equities; see the caveat below |
| `apply_shock_simd` (SIMD phases only) | **1.14 µs p50** | 2 SIMD sweeps + scalar cascade pass |
| Ingress (uWS frame to ring) | ~200 ns | **estimate, not instrumented** |
| Egress (TickDelta to socket) | ~500 ns | **estimate, not instrumented** |

> **Read the cascade number carefully.** `run_cascade_tick()` iterates to quiescence, so
> its cost is a function of the shock, not a constant, and the spread is roughly 700x. A
> -30% equities shock on the 500-node graph settles in **2 rounds and triggers zero
> defaults**, so 4.16 µs buys an equilibrium check, not a contagion cascade. Severe shocks
> run for thousands of rounds and cost milliseconds. Any single latency number for this
> function is meaningless without the shock vector.
>
> The `MAX_CASCADE_ROUNDS` cap was also **truncating results**, not just bounding runtime.
> Sweeping it until default counts stop changing shows a -80% equities shock converging at
> **30 defaults with the last at round 4143**, against the 22 the old 1024 cap reported.
> Default counts published under that cap were lower bounds.

### Which channel actually causes the defaults

The cascade has two ways for one firm's trouble to reach another: option delta hedging
pushes orders into the equities book and moves the **price**, and a stressed or defaulting
firm pushes risk along its CSR debt edges to its **counterparties**. Each was disabled
behind a compile-time flag and all nine stress scenarios re-run, with a fourth build
disabling both to supply a baseline for firms that fail on the direct shock alone.

| Scenario | Full | No gamma | No counterparty | Neither |
|---|---|---|---|---|
| -80% equities | 30 | **30** | 0 | 0 |
| -50% all classes | 216 | **216** | 189 | 189 |
| -90% all classes | 500 | 500 | 500 | 500 |
| other six scenarios | 0 | 0 | 0 | 0 |

**The gamma-hedging channel accounts for 0% of defaults in every scenario.** Disabling it
leaves every count and every last-default round unchanged. The options book, the AVX2
Black-Scholes kernel and the delta hedging move prices, but never enough to break a firm
that would not have broken anyway. Counterparty contagion accounts for all 30 defaults
under -80% equities and 27 of 216 under a -50% shock across all classes; the rest fail on
the direct mark-to-market. The interaction term is exactly zero in all nine scenarios, so
the channels are additive and these shares are meaningful.

The engine's data flow is strictly segmented across three CPU-pinned threads communicating via `std::atomic` cursors with explicit memory ordering (`memory_order_release`/`memory_order_acquire`).

* **Ingress (Thread 1 - Core 1):** uWebSockets event loop ingests data via binary frame parsing. Payloads are copied directly into a 56-byte `#pragma pack(1)` struct via `memcpy`. Zero string parsing, zero JSON, zero dynamic allocation in our code, though the uWS event loop itself blocks in `epoll`/`kqueue` and allocates, so the *lock-free* property applies to the Compute→Broadcast path, not to ingress.
* **Compute (Thread 2 - Core 2):** Spins via PAUSE/YIELD instructions. The risk cascade computes in three phases: SIMD Exposure Update (2500 FMA ops), SIMD NAV Recomputation (ILP-optimized addition tree with a 2-cycle critical path), and Cascade BFS (Scalar with explicit `__builtin_prefetch` fetching 4 cache lines ahead). See the table above for measured figures. Timings come from `read_timestamp()`, whose tick rate is resolved at startup by `calibrate_timestamp_clock()`, neither RDTSC nor `CNTVCT_EL0` ticks at the core clock, so durations are never derived from an assumed GHz.
* **Egress (Thread 3 - Core 3):** Broadcasts state updates via TCP (uWS) and UDP POSIX `sendto` multicast. Network serialization is strictly zero-copy, utilizing scatter-gather I/O (`sendmsg` with `iovec` arrays) directly into the socket buffer. Estimated latency: **~500ns** (not instrumented).

## Memory & Cache Architecture

Dynamic memory allocation is entirely banned on the hot path. The whole working set is statically allocated at 508 KB, which is small enough to sit close to the core but not with room to spare: a typical x86-64 L2 is 512 KB to 2 MB per core, so on the smaller end it only just fits.

* **Zero-Allocation .bss Footprint:** Sizes below are `sizeof()` on the shipped structs, not estimates.

  | Structure | Size |
  |---|---|
  | `CSRGraph` (`NodeData` 43.0 KB + `CSREdges` 98.0 KB) | 141.1 KB |
  | Both ring buffers (`ShockPayload` + `TickDelta`) | 128.1 KB |
  | `CLOBEngine`, of which BBO double-buffers are 64.0 KB | 224.4 KB |
  | `OptionsBook` | 14.0 KB |
  | **Total hot-path memory** | **507.6 KB** |

  The CLOB dominates: five books at 256 bid and 256 ask levels, each `PriceLevel` padded to a 64-byte cache line, is 160 KB of price levels alone.
* **CSR Struct-of-Arrays (SoA) Layout:** The graph utilizes a Compressed Sparse Row (CSR) format mapping `row_ptr`, `col_idx`, and `weight` arrays (98 KB total). The SoA layout ensures sequential access patterns that saturate the hardware prefetcher, achieving 100% cache-line utilization with zero padding waste. Graph traversal executes in strict $O(V + E)$ time.
* **Explicit prefetching helps one workload and hurts another.** The eight `__builtin_prefetch` calls in `simd_engine.hpp` were ablated against an otherwise identical build (`-DOPTIRISK_NO_PREFETCH`), both binaries compiled and run in the same job so the comparison is same-machine. On the isolated SIMD sweep (`apply_shock_simd`) they are worth **+17 to +19% on AMD EPYC** and **neutral on Intel Xeon**. On the full cascade they are a consistent **loss of 8 to 12% on both**. The plausible reading: the sweeps are linear SoA strides the hardware prefetcher already handles, and inside the full cascade the hints evict lines the CLOB and options book still need. Retained behind the flag so the result stays reproducible either way.
* **False Sharing Prevention:** Cross-core cache-line bouncing is eliminated by padding all atomic cursors, CLOB buffers, and SPSC ring buffer slots with explicit `alignas(64)` directives. Each pinned thread owns exclusive cache lines.

## Central Limit Order Book

The CLOB simulates forced liquidations across 5 global asset classes (Equities, Real Estate, Crypto, Treasuries, Corp Bonds) with realistic baseline prices and depth profiles. It models the slippage penalty incurred when a counterparty default forces a fire-sale into illiquid markets, the core mechanism by which contagion propagates through the network.

* **Depth-Walking Liquidation:** `market_sell` / `market_buy` walk the bid/ask stacks level-by-level, filling against available depth and returning a `FillResult` containing average fill price, total proceeds, slippage vs. pre-trade mid, and the count of price levels consumed. The hot path is `__attribute__((always_inline))` annotated and `[[unlikely]]`-hinted on the empty-book guard. (The depth-walking loop itself is branch-driven, the branchless work lives in the Black-Scholes kernel, below.) Measured at **20 ns p50 / 40 ns p99** per fill, sustaining 4.57 M fills/s.
* **Lazy Head-Increment Matching:** Consumed price levels are not erased mid-array (which would trigger $O(N)$ shifts and cache invalidation). Instead, a `bids_head` / `asks_head` pointer increments past depleted levels, leaving fills $O(\text{levels consumed})$ with zero memory movement. **Verified empirically in both directions:** cost is flat in book depth (10 ns p50 at depth 8 and at depth 256, one level consumed) and linear in levels consumed (10 ns at 1 level, 350 ns at 256).
* **Ping-Pong BBO Double-Buffer:** The `CLOBEngine` maintains two `alignas(64)` BBO update buffers and an `std::atomic<uint8_t> active_buffer_idx`. The compute thread writes into the active buffer; the broadcast thread reads the inactive one. Buffer flips use `memory_order_release` on the store and `memory_order_acquire` on the read, guaranteeing the broadcast thread never observes a half-written packet stream. This eliminates the standard producer/consumer mutex without sacrificing correctness. Flip-to-observed latency measures **50 ns p50 / 80 ns p99**. That is in-process visibility between two threads sharing the buffer, not a publish: the shipped path then hands the span to `UdpPublisher::broadcast_bbo()`, whose `sendmsg` to the multicast group costs **5.40 µs p50**. Fill-to-wire is dominated by the syscall, and the ping-pong buffer is under 1% of it.
* **Cache-Aligned Storage:** Both `OrderBook` and `PriceLevel` are `alignas(64)` to match cache-line boundaries. The 5-asset book array is statically allocated as `std::array<OrderBook, 5>`, fully embedded in the engine's `.bss` footprint with zero heap touches.
* **Macro Shock Operator:** `apply_macro_shock(delta)` multiplies every active price level by `(1 + delta)` in a tight loop, modeling instantaneous market-wide repricing events without rebuilding the book.

## Hardware & Execution Mechanics

Pipeline stalls and branch mispredictions are lethal to microsecond determinism. OptiRisk utilizes explicit hardware-level control flows:

* **AVX2/FMA3 Intrinsics:** The engine leverages pipelined FMA instructions (`_mm256_fmadd_pd`). The 8-lane AVX2 kernel computes option **delta** (not price, and no second-order Greeks): fast logarithms via 6 FMAs, Normal CDF via the Abramowitz & Stegun 7.1.28 degree-6 $(1 + a_1x + \dots + a_6x^6)^{-16}$ form, and division via 14-bit reciprocal approximation (`_mm256_rcp_ps`) plus one Newton-Raphson refinement, bypassing 20-cycle hardware division. Measured at **1,053 M options/s single-core, 12.9x a double-precision `std::erfc` scalar baseline**. That total decomposes into **1.52x from cheaper math** (f64 libm to f32 polynomial approximations) and **8.50x from vectorization** (1 lane to 8), measured with a scalar f32 rung using the kernel's exact approximations: the hand-rolled approximation machinery is worth far less than the lanes. Accuracy against that f64 reference over a 9,072-point (spot, strike, vol, rate, expiry) grid: **max absolute delta error $7.4\times10^{-7}$**, mean $1.4\times10^{-7}$. The fast-log series holds to $1.2\times10^{-7}$ over $S/K \in [0.7, 1.5]$ and the refined reciprocal to $1.6\times10^{-7}$ relative.
  * The polynomial is evaluated in `float`, and $y = |d_1|/\sqrt2$ is saturated at 10 before it. Without that clamp the 16th power overflows `float32` to $+\infty$, `_mm256_rcp_ps(\infty)` returns 0, and the Newton-Raphson step yields $2 - \infty \times 0 = $ **NaN**, which it did for 126 of those 9,072 points, concentrated in deep-OTM short-dated options. Since $\mathrm{erfc}(10) \approx 2\times10^{-45}$ is below `float32`'s smallest normal, the clamp returns the exact single-precision limit and changes no previously-finite result.
* **Branchless Arithmetic:** Absolute values use bitwise AND masking (`0x7FFFFFFF`), Call/Put deltas resolve via blends (`_mm256_blendv_ps`), and ring indexing uses bitmask modulo (`seq & RING_MASK`). The *arithmetic* is branchless; the enclosing loop is not. `objdump` of the compiled kernel shows **12 conditional jumps**, including a data-dependent early-out for all-zero position groups. That count rose from 4 when the scalar tail loop was moved out of the `#else` so that x86 prices every option rather than silently skipping the final partial group of 8; carrying both loops costs branches, and pricing all 500 options is worth more than the lower number. Hot paths carry `[[likely]]` / `[[unlikely]]` hints.
* **Lock-Free Concurrency:** The LMAX Disruptor pattern ensures zero mutexes and zero OS-level blocking on the compute path. Producer back-pressure spin-waits when the ring buffer reaches a 1024-slot delta, preventing unbounded memory growth. Measured against a `std::mutex` + `std::queue` + `condition_variable` transport carrying the identical payload at the identical arrival rate, bounded to the same 1024 slots: **50 ns vs 3,055 ns at p50 (61x)** and **80 ns vs 9,205 ns at p99 (115x)**, with saturated throughput of **41.6 M vs 6.8 M events/s (6.1x)**. The ratio moves with the runner (the mutex baseline is the volatile half), but the lock-free path has been one to two orders of magnitude faster on every machine measured.

## Quantitative Math & Numerical Stability

To handle ill-conditioned, real-world financial data, OptiRisk implements mathematical stabilizers explicitly engineered to prevent accumulation errors and catastrophic cancellations.

* **Sinkhorn-Knopp Matrix Balancing:** Maximum entropy bilateral exposure inference is solved via an Alternating Scaling (RAS) method. Operating at $O(n^2 \times \text{iterations})$, the $500 \times 500$ dense matrix is run for a fixed `max_iter=200` sweeps. Note there is no convergence test in the loop: it performs 200 iterations unconditionally, and residual convergence is not measured. Division-by-zero is avoided by replacing exactly-zero row and column sums with 1.0 before scaling, not by a threshold clamp.
* **Merton Distance-to-Default:** The structural credit model computes PD bounds via rank-based volatilities (ranging from 15% for mega-banks to 80% for volatile firms). Asset and liability vectors are clamped to prevent $\log(0)$ instability.
* **Student-T Copula Margins:** Tail risk is modeled by transforming Gaussian copulas to inverse Student-T distributions. Fat tails for crypto asset classes are aggressively modeled at $df=2.5$, while standard equities and real estate default to $df=4.0$. 
* **Welford's Online VaR:** `MC_PATHS = 1024` paths are executed across 500 nodes ($O(\text{paths} \times \text{nodes})$). Welford's algorithm is single-pass and online, updating mean and $M_2$ per sample, which computes variance without catastrophic cancellation and without storing the paths. Zero heap allocation: state lives in stack-resident `std::array`s.

## Frontend: the cinematic map

Located in `frontend/`. Next.js 15 (App Router) + TypeScript + Tailwind.

### Rendering stack

- **MapLibre**: dark basemap providing geographic context
- **deck.gl**: GPU-accelerated layers on top of the map (nodes, edges, labels, hub blobs, focus highlights). See `components/map/layers/`.
- **React**: manages app structure only; it never re-renders the graph itself.

> Note: this project uses MapLibre + deck.gl, **not** React Three Fiber. The graph is a geo graph, not a free-floating force graph.

### State = Zustand stores, split by concern (`frontend/store/`)

| Store | Responsibility |
|-------|----------------|
| `connectionStore.ts` | WebSocket / SSE status, last message time |
| `graphStore.ts`      | Nodes, edges, which entities changed this tick |
| `simulationStore.ts` | Phase (`pre_shock` → `shock_triggered` → `cascade_running` → `cascade_complete`), current tick, event log, VaR report |
| `uiStore.ts`         | Hovered node, selected node, focused city |

### Transport: SSE, not raw browser WebSocket

The browser does **not** talk to C++ directly. Instead:

1. `frontend/services/websocket.ts` opens an `EventSource('/api/stream')`.
2. `frontend/app/api/stream/route.ts` (a Next.js Node route) holds the real WebSocket to the C++ backend on `localhost:8080` and base64-forwards each binary frame as an SSE `data:` line.
3. `frontend/lib/binary/decodeDelta.ts` decodes those bytes back into typed objects via `DataView`.

Why this dance? SSE rides on plain HTTP/1.1, which Next.js dev server, every CDN, and every WSL2 / VPN port forwarder handles correctly. No CORS, no second port to expose, no mixed-content rules.

Outbound shocks take the symmetric path: the browser POSTs a base64-encoded `ShockPayload` to `/api/shock`, which forwards it to the C++ engine over the same upstream WebSocket.

### Display pacing

The backend ticks at ~10 Hz, which would make the cascade finish in <1 second, too fast for a human to follow. So `websocket.ts` buffers incoming `TickDelta`s **grouped by `tick_seq`** and drains one batch every 400 ms. The backend keeps computing at full speed; the user sees a wave.

### The chat / AI layer

`frontend/app/api/chat/route.ts` accepts natural-language prompts ("simulate a 2008-style collapse on JPMorgan"), sends them to an LLM that emits a structured `trigger_market_shock(...)` call, packs the result into the C++ binary `ShockPayload`, and POSTs it to `/api/shock`. If the LLM is unavailable, a local regex parser keeps the chatbox usable fully offline.

### UI shell

`app/page.tsx` is short and tells the whole story: full-screen `<MapContainer />` underneath, with floating HUD panels, `TopControls`, `NodeInfoCard` / `CityHubPanel`, `ChatPanel`, `StatusBar`. Dark, Obsidian-inspired aesthetic, all Tailwind.

---

## The key seam: one binary contract, two languages

`network/wire_protocol.hpp` (C++) and `lib/binary/schema.ts` + `lib/binary/decodeDelta.ts` (TypeScript) describe the **exact same bytes**. Change one, you must change the other. Field offsets, sizes, and message-type enums are kept in lock-step.

```text
TickDelta       = 56 bytes   MsgType 0x02
VaRReport       = 16 bytes   MsgType 0x07
MarketAnchors   = 40 bytes   MsgType 0x04
ShockPayload    = 56 bytes   MsgType 0x01   (header is 4 bytes)
```

---

## Tech Stack

| Layer     | Technology                                | Purpose                            |
|-----------|-------------------------------------------|------------------------------------|
| Engine    | C++23, POSIX threads, AVX2 / FMA3         | Risk computation, Monte Carlo VaR  |
| Memory    | CSR SoA, `std::array`, stack allocation   | Cache-optimal graph storage        |
| Pipeline  | LMAX Disruptor (custom, lock-free)        | Inter-thread comms                 |
| Network   | uWS WebSocket (binary frames) + UDP mcast | Sub-millisecond data delivery      |
| Bridge    | Next.js Node routes (SSE + POST)          | Browser-friendly transport         |
| Frontend  | Next.js 15, MapLibre, deck.gl, Zustand    | Geo visualization + state mgmt     |
| Styling   | Tailwind CSS (dark mode only)             | UI framework                       |
| Chat      | LLM-routed natural-language → binary shock | Operator interface                 |

---

## Getting Started

### Backend

```bash
cd backend
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./optirisk
```

The engine listens on WS port `8080` and reads its initial market state from `optirisk_memory.bin` (generated by `scripts/infer_network.py`).

### Frontend

```bash
cd frontend
npm install
npm run dev
```

Open [http://localhost:3000](http://localhost:3000). The page boots the SSE bridge automatically.

---

## Suggested code-tour order

1. `app/page.tsx`: the entire UI shell in ~50 lines.
2. `README.md` architecture diagram, how data flows.
3. `backend/src/main.cpp`: the three threads, one file.
4. `backend/src/memory/csr_graph.hpp`: the whole 500-node network in one stack-allocated struct.
5. `backend/src/market/order_book.hpp`: the CLOB, depth-walking matching, and ping-pong BBO double-buffer.
6. `backend/src/network/wire_protocol.hpp` next to `frontend/lib/binary/schema.ts`, the binary contract.
7. Run it, click a shock, watch the cascade.

---

## Team

Built in 36 hours for HackPrinceton at Princeton University.

## License

MIT
