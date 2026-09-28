# OptiRisk

**Real-time counterparty risk simulation at HFT speeds.**

OptiRisk is a high-performance counterparty credit-risk engine that models cascading default propagation across a 500-node financial network. Architected identically to a Tier-1 trading system, it features a bare-metal C++23 backend and a strict binary wire contract to a Next.js/WebGL frontend. 

The system operates with a strict zero-allocation hot path built on LMAX Disruptor ring
buffers, AVX2 vectorization, and cache-optimal Struct-of-Arrays (SoA) layouts. Every
performance figure below is measured by the suite in `bench/`; see **[BENCHMARKS.md](BENCHMARKS.md)**
for the hardware, methodology, and raw per-sample distributions.

## The Critical Path

Measured on a GitHub-hosted x86-64 runner (AVX2, GCC 13.3, threads pinned), with the
machine's own noise floor characterised first so the percentiles are known to be signal
rather than interference. Ingress and egress remain **engineering estimates** — they are
not instrumented, and are labelled as such.

| Stage | Measured | Notes |
|---|---|---|
| Disruptor ring handoff | **36 ns p50 / 49 ns p99** | SPSC, paced to queue depth ~1; 0.0% p99 spread over 5 runs |
| Full pipeline (shock in → TickDelta published) | **9.6 µs p50 / 16.3 µs p99** | 500 nodes / 7500 edges, −30% equities |
| `run_cascade_tick` alone | **7.2 µs p50** | same shock; see the caveat below |
| `apply_shock_simd` (SIMD phases only) | **1.7 µs p50** | 2 SIMD sweeps + scalar cascade pass |
| Ingress (uWS frame → ring) | ~200 ns | **estimate, not instrumented** |
| Egress (TickDelta → socket) | ~500 ns | **estimate, not instrumented** |

> **Read the cascade number carefully.** `run_cascade_tick()` iterates to quiescence, so
> its cost is a function of the shock, not a constant. A −30% equities shock on the
> 500-node graph settles in **2 rounds and triggers zero defaults** — 7.2 µs buys you an
> equilibrium check, not a contagion cascade. Severe shocks (−80% equities, or −50%
> across all five classes) run to the `MAX_CASCADE_ROUNDS = 1024` cap and cost **2.5 ms**.
> Any single latency number for this function is meaningless without the shock vector.

The engine's data flow is strictly segmented across three CPU-pinned threads communicating via `std::atomic` cursors with explicit memory ordering (`memory_order_release`/`memory_order_acquire`).

* **Ingress (Thread 1 - Core 1):** uWebSockets event loop ingests data via binary frame parsing. Payloads are copied directly into a 56-byte `#pragma pack(1)` struct via `memcpy`. Zero string parsing, zero JSON, zero dynamic allocation in our code — though the uWS event loop itself blocks in `epoll`/`kqueue` and allocates, so the *lock-free* property applies to the Compute→Broadcast path, not to ingress.
* **Compute (Thread 2 - Core 2):** Spins via PAUSE/YIELD instructions. The risk cascade computes in three phases: SIMD Exposure Update (2500 FMA ops), SIMD NAV Recomputation (ILP-optimized addition tree with a 2-cycle critical path), and Cascade BFS (Scalar with explicit `__builtin_prefetch` fetching 4 cache lines ahead). See the table above for measured figures. Timings come from `read_timestamp()`, whose tick rate is resolved at startup by `calibrate_timestamp_clock()` — neither RDTSC nor `CNTVCT_EL0` ticks at the core clock, so durations are never derived from an assumed GHz.
* **Egress (Thread 3 - Core 3):** Broadcasts state updates via TCP (uWS) and UDP POSIX `sendto` multicast. Network serialization is strictly zero-copy, utilizing scatter-gather I/O (`sendmsg` with `iovec` arrays) directly into the socket buffer. Estimated latency: **~500ns** (not instrumented).

## Memory & Cache Architecture

Dynamic memory allocation is entirely banned on the hot path. The system is designed to fit entirely within the L2 cache of modern processors to prevent main-memory roundtrips.

* **Zero-Allocation .bss Footprint:** The entire graph is statically allocated (158 KB), alongside the ring buffers (128 KB), CLOB double-buffers (64 KB), and the Options book (16 KB). The total hot-path memory is roughly **382 KB**.
* **CSR Struct-of-Arrays (SoA) Layout:** The graph utilizes a Compressed Sparse Row (CSR) format mapping `row_ptr`, `col_idx`, and `weight` arrays (98 KB total). The SoA layout ensures sequential access patterns that saturate the hardware prefetcher, achieving 100% cache-line utilization with zero padding waste. Graph traversal executes in strict $O(V + E)$ time.
* **Explicit prefetching helps one workload and hurts another.** The eight `__builtin_prefetch` calls in `simd_engine.hpp` were ablated against an otherwise identical build (`-DOPTIRISK_NO_PREFETCH`), both binaries compiled and run in the same job so the comparison is same-machine. On the isolated SIMD sweep (`apply_shock_simd`) they are worth **+17 to +19% on AMD EPYC** and **neutral on Intel Xeon**. On the full cascade they are a consistent **loss of 8–12% on both**. The plausible reading: the sweeps are linear SoA strides the hardware prefetcher already handles, and inside the full cascade the hints evict lines the CLOB and options book still need. Retained behind the flag so the result stays reproducible either way.
* **False Sharing Prevention:** Cross-core cache-line bouncing is eliminated by padding all atomic cursors, CLOB buffers, and SPSC ring buffer slots with explicit `alignas(64)` directives. Each pinned thread owns exclusive cache lines.

## Central Limit Order Book

The CLOB simulates forced liquidations across 5 global asset classes (Equities, Real Estate, Crypto, Treasuries, Corp Bonds) with realistic baseline prices and depth profiles. It models the slippage penalty incurred when a counterparty default forces a fire-sale into illiquid markets — the core mechanism by which contagion propagates through the network.

* **Depth-Walking Liquidation:** `market_sell` / `market_buy` walk the bid/ask stacks level-by-level, filling against available depth and returning a `FillResult` containing average fill price, total proceeds, slippage vs. pre-trade mid, and the count of price levels consumed. The hot path is `__attribute__((always_inline))` annotated and `[[unlikely]]`-hinted on the empty-book guard. (The depth-walking loop itself is branch-driven — the branchless work lives in the Black-Scholes kernel, below.) Measured at **50 ns p50 / 110 ns p99** per fill.
* **Lazy Head-Increment Matching:** Consumed price levels are not erased mid-array (which would trigger $O(N)$ shifts and cache invalidation). Instead, a `bids_head` / `asks_head` pointer increments past depleted levels, leaving fills $O(\text{levels consumed})$ with zero memory movement. **Verified empirically in both directions:** cost is flat in book depth (14 ns at depth 8 and at depth 256, one level consumed) and linear in levels consumed (13 ns at 1 level → 549 ns at 256).
* **Ping-Pong BBO Double-Buffer:** The `CLOBEngine` maintains two `alignas(64)` BBO update buffers and an `std::atomic<uint8_t> active_buffer_idx`. The compute thread writes into the active buffer; the broadcast thread reads the inactive one. Buffer flips use `memory_order_release` on the store and `memory_order_acquire` on the read, guaranteeing the broadcast thread never observes a half-written packet stream. This eliminates the standard producer/consumer mutex without sacrificing correctness. Flip-to-observed latency measures **45 ns p50 / 57 ns p99**.
* **Cache-Aligned Storage:** Both `OrderBook` and `PriceLevel` are `alignas(64)` to match cache-line boundaries. The 5-asset book array is statically allocated as `std::array<OrderBook, 5>`, fully embedded in the engine's `.bss` footprint with zero heap touches.
* **Macro Shock Operator:** `apply_macro_shock(delta)` multiplies every active price level by `(1 + delta)` in a tight loop, modeling instantaneous market-wide repricing events without rebuilding the book.

## Hardware & Execution Mechanics

Pipeline stalls and branch mispredictions are lethal to microsecond determinism. OptiRisk utilizes explicit hardware-level control flows:

* **AVX2/FMA3 Intrinsics:** The engine leverages pipelined FMA instructions (`_mm256_fmadd_pd`). The 8-lane AVX2 kernel computes option **delta** (not price, and no second-order Greeks): fast logarithms via 6 FMAs, Normal CDF via the Abramowitz & Stegun 7.1.28 degree-6 $(1 + a_1x + \dots + a_6x^6)^{-16}$ form, and division via 14-bit reciprocal approximation (`_mm256_rcp_ps`) plus one Newton-Raphson refinement, bypassing 20-cycle hardware division. Measured at **640 M options/s single-core, 11.7× a double-precision `std::erfc` scalar baseline**. Accuracy against that f64 reference over a 9,072-point (spot, strike, vol, rate, expiry) grid: **max absolute delta error $7.4\times10^{-7}$**, mean $1.4\times10^{-7}$. The fast-log series holds to $1.2\times10^{-7}$ over $S/K \in [0.7, 1.5]$ and the refined reciprocal to $1.6\times10^{-7}$ relative.
  * The polynomial is evaluated in `float`, and $y = |d_1|/\sqrt2$ is saturated at 10 before it. Without that clamp the 16th power overflows `float32` to $+\infty$, `_mm256_rcp_ps(\infty)` returns 0, and the Newton-Raphson step yields $2 - \infty \times 0 = $ **NaN** — which it did for 126 of those 9,072 points, concentrated in deep-OTM short-dated options. Since $\mathrm{erfc}(10) \approx 2\times10^{-45}$ is below `float32`'s smallest normal, the clamp returns the exact single-precision limit and changes no previously-finite result.
* **Branchless Arithmetic:** Absolute values use bitwise AND masking (`0x7FFFFFFF`), Call/Put deltas resolve via blends (`_mm256_blendv_ps`), and ring indexing uses bitmask modulo (`seq & RING_MASK`). The *arithmetic* is branchless; the enclosing loop is not — `objdump` of the compiled kernel shows **4 conditional jumps**, including a data-dependent early-out for all-zero position groups. Hot paths carry `[[likely]]` / `[[unlikely]]` hints.
* **Lock-Free Concurrency:** The LMAX Disruptor pattern ensures zero mutexes and zero OS-level blocking on the compute path. Producer back-pressure spin-waits when the ring buffer reaches a 1024-slot delta, preventing unbounded memory growth. Measured against a `std::mutex` + `std::queue` + `condition_variable` transport carrying the identical payload at the identical arrival rate: **36 ns vs 2,342 ns at p50 (65×)** and **49 ns vs 6,838 ns at p99 (140×)**, with saturated throughput of **52.0 M vs 5.5 M events/s (9.4×)**.

## Quantitative Math & Numerical Stability

To handle ill-conditioned, real-world financial data, OptiRisk implements mathematical stabilizers explicitly engineered to prevent accumulation errors and catastrophic cancellations.

* **Sinkhorn-Knopp Matrix Balancing:** Maximum entropy bilateral exposure inference is solved via a convex Alternating Scaling (RAS) method. Operating at $O(n^2 \times \text{iterations})$, the $500 \times 500$ dense matrix converges in 200 iterations. To prevent division-by-zero, row and column sums are strictly clamped to $\ge 1e^{-3}$.
* **Merton Distance-to-Default:** The structural credit model computes PD bounds via rank-based volatilities (ranging from 15% for mega-banks to 80% for volatile firms). Asset and liability vectors are clamped to prevent $\log(0)$ instability.
* **Student-T Copula Margins:** Tail risk is modeled by transforming Gaussian copulas to inverse Student-T distributions. Fat tails for crypto asset classes are aggressively modeled at $df=2.5$, while standard equities and real estate default to $df=4.0$. 
* **Welford's Online VaR:** 1024 Monte Carlo paths are executed across 500 nodes ($O(\text{paths} \times \text{nodes})$). Welford's two-pass algorithm computes the moving variance without catastrophic cancellation and with absolute zero heap allocation, utilizing purely stack-resident arrays for state management.

## Frontend — the cinematic map

Located in `frontend/`. Next.js 15 (App Router) + TypeScript + Tailwind.

### Rendering stack

- **MapLibre** — dark basemap providing geographic context
- **deck.gl** — GPU-accelerated layers on top of the map (nodes, edges, labels, hub blobs, focus highlights). See `components/map/layers/`.
- **React** — manages app structure only; it never re-renders the graph itself.

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

The backend ticks at ~10 Hz, which would make the cascade finish in <1 second — too fast for a human to follow. So `websocket.ts` buffers incoming `TickDelta`s **grouped by `tick_seq`** and drains one batch every 400 ms. The backend keeps computing at full speed; the user sees a wave.

### The chat / AI layer

`frontend/app/api/chat/route.ts` accepts natural-language prompts ("simulate a 2008-style collapse on JPMorgan"), sends them to an LLM that emits a structured `trigger_market_shock(...)` call, packs the result into the C++ binary `ShockPayload`, and POSTs it to `/api/shock`. If the LLM is unavailable, a local regex parser keeps the chatbox usable fully offline.

### UI shell

`app/page.tsx` is short and tells the whole story: full-screen `<MapContainer />` underneath, with floating HUD panels — `TopControls`, `NodeInfoCard` / `CityHubPanel`, `ChatPanel`, `StatusBar`. Dark, Obsidian-inspired aesthetic, all Tailwind.

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

1. `app/page.tsx` — the entire UI shell in ~50 lines.
2. `README.md` architecture diagram — how data flows.
3. `backend/src/main.cpp` — the three threads, one file.
4. `backend/src/memory/csr_graph.hpp` — the whole 500-node network in one stack-allocated struct.
5. `backend/src/market/order_book.hpp` — the CLOB, depth-walking matching, and ping-pong BBO double-buffer.
6. `backend/src/network/wire_protocol.hpp` next to `frontend/lib/binary/schema.ts` — the binary contract.
7. Run it, click a shock, watch the cascade.

---

## Team

Built in 36 hours for HackPrinceton at Princeton University.

## License

MIT
