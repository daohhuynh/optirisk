# OptiRisk

**Real-time counterparty risk simulation at HFT speeds.**

OptiRisk is a high-performance counterparty credit-risk engine that models cascading default propagation across a 500-node financial network. Architected identically to a Tier-1 trading system, it features a bare-metal C++23 backend and a strict binary wire contract to a Next.js/WebGL frontend. 

The system operates with a strict zero-allocation hot path, achieving an end-to-end network-to-execution latency of **~11μs**, leveraging LMAX Disruptor ring buffers, AVX2 vectorization, and cache-optimal Struct-of-Arrays (SoA) layouts.

## The Critical Path: ~11μs End-to-End Latency

Compute-stage timings below are **measured** (see Benchmarks); the ingress and egress
figures are **engineering estimates** and have not been instrumented end-to-end.

The engine's data flow is strictly segmented across three CPU-pinned threads communicating via `std::atomic` cursors with explicit memory ordering (`memory_order_release`/`memory_order_acquire`).

* **Ingress (Thread 1 - Core 1):** uWebSockets event loop ingests data via binary frame parsing. Payloads are copied directly into a 56-byte `#pragma pack(1)` struct via `memcpy`. The system utilizes zero string parsing, zero JSON overhead, and zero dynamic allocation. Estimated latency: **~200ns** (not instrumented).
* **Compute (Thread 2 - Core 2):** Spins via PAUSE/YIELD instructions. The risk cascade computes in three phases: SIMD Exposure Update (2500 FMA ops), SIMD NAV Recomputation (ILP-optimized addition tree with a 2-cycle critical path), and Cascade BFS (Scalar with explicit `__builtin_prefetch` fetching 4 cache lines ahead). A full `run_cascade_tick()` measures **~10μs** steady-state (9.0–11.0μs over 7 runs; ~18μs on the first, cold-cache run) for a −30% equities shock on a 500-node / 7500-edge graph, measured on Apple Silicon via the NEON path. The SIMD phases alone are **~414ns**, and the Disruptor ring handoff is **~125ns p50 / 208ns p99** measured unsaturated at queue depth 1. Timings come from `read_timestamp()`, whose tick rate is resolved at startup by `calibrate_timestamp_clock()` — note that neither RDTSC nor `CNTVCT_EL0` ticks at the core clock, so durations are never derived from an assumed GHz.
* **Egress (Thread 3 - Core 3):** Broadcasts state updates via TCP (uWS) and UDP POSIX `sendto` multicast. Network serialization is strictly zero-copy, utilizing scatter-gather I/O (`sendmsg` with `iovec` arrays) directly into the socket buffer. Estimated latency: **~500ns** (not instrumented).

## Memory & Cache Architecture

Dynamic memory allocation is entirely banned on the hot path. The system is designed to fit entirely within the L2 cache of modern processors to prevent main-memory roundtrips.

* **Zero-Allocation .bss Footprint:** The entire graph is statically allocated (158 KB), alongside the ring buffers (128 KB), CLOB double-buffers (64 KB), and the Options book (16 KB). The total hot-path memory is roughly **382 KB**.
* **CSR Struct-of-Arrays (SoA) Layout:** The graph utilizes a Compressed Sparse Row (CSR) format mapping `row_ptr`, `col_idx`, and `weight` arrays (98 KB total). The SoA layout ensures sequential access patterns that perfectly saturate the hardware prefetcher, achieving 100% cache-line utilization with zero padding waste. Graph traversal executes in strict $O(V + E)$ time.
* **False Sharing Prevention:** Cross-core cache-line bouncing is eliminated by padding all atomic cursors, CLOB buffers, and SPSC ring buffer slots with explicit `alignas(64)` directives. Each pinned thread owns exclusive cache lines.

## Central Limit Order Book

The CLOB simulates forced liquidations across 5 global asset classes (Equities, Real Estate, Crypto, Treasuries, Corp Bonds) with realistic baseline prices and depth profiles. It models the slippage penalty incurred when a counterparty default forces a fire-sale into illiquid markets — the core mechanism by which contagion propagates through the network.

* **Depth-Walking Liquidation:** `market_sell` / `market_buy` walk the bid/ask stacks level-by-level, filling against available depth and returning a `FillResult` containing average fill price, total proceeds, slippage vs. pre-trade mid, and the count of price levels consumed. The hot path is `__attribute__((always_inline))` annotated and `[[unlikely]]`-hinted on the empty-book guard. (The depth-walking loop itself is branch-driven — the branchless work lives in the Black-Scholes kernel, below.)
* **Lazy Head-Increment Matching:** Consumed price levels are not erased mid-array (which would trigger $O(N)$ shifts and cache invalidation). Instead, a `bids_head` / `asks_head` pointer increments past depleted levels, leaving fills $O(\text{levels consumed})$ with zero memory movement.
* **Ping-Pong BBO Double-Buffer:** The `CLOBEngine` maintains two `alignas(64)` BBO update buffers and an `std::atomic<uint8_t> active_buffer_idx`. The compute thread writes into the active buffer; the broadcast thread reads the inactive one. Buffer flips use `memory_order_release` on the store and `memory_order_acquire` on the read, guaranteeing the broadcast thread never observes a half-written packet stream. This eliminates the standard producer/consumer mutex without sacrificing correctness.
* **Cache-Aligned Storage:** Both `OrderBook` and `PriceLevel` are `alignas(64)` to match cache-line boundaries. The 5-asset book array is statically allocated as `std::array<OrderBook, 5>`, fully embedded in the engine's `.bss` footprint with zero heap touches.
* **Macro Shock Operator:** `apply_macro_shock(delta)` multiplies every active price level by `(1 + delta)` in a tight loop, modeling instantaneous market-wide repricing events without rebuilding the book.

## Hardware & Execution Mechanics

Pipeline stalls and branch mispredictions are lethal to microsecond determinism. OptiRisk utilizes explicit hardware-level control flows:

* **AVX2/FMA3 Intrinsics:** The engine leverages pipelined FMA instructions (`_mm256_fmadd_pd`). The 8-lane AVX2 Black-Scholes pricing kernel computes fast logarithms via 6 FMA instructions, approximates Normal CDF using the Abramowitz & Stegun 7.1.28 polynomial (the degree-6 $(1 + a_1x + \dots + a_6x^6)^{-16}$ form). Its published bound is $|\epsilon| \le 3\times10^{-7}$, which holds in double precision (measured max $2.60\times10^{-7}$ over $x \in [0,6]$); the kernel evaluates it in `float`, where realized error is $\approx 3.4\times10^{-6}$ — ample for a delta hedge, but not the textbook figure. and handles division via 14-bit reciprocal approximation (`_mm256_rcp_ps`) followed by a single Newton-Raphson refinement for a 7-cycle yield (bypassing 20-cycle hardware division).
* **Branchless Execution:** The system explicitly avoids branch mispredictions. Absolute values utilize bitwise AND masking (`0x7FFFFFFF`). Call/Put deltas are resolved via CMOV-style blends (`_mm256_blendv_ps`). Ring buffer indexing utilizes bitmask modulo (`seq & RING_MASK`). All hot paths are annotated with `[[likely]]` / `[[unlikely]]` compiler hints.
* **Lock-Free Concurrency:** The LMAX Disruptor pattern ensures zero mutexes and zero OS-level blocking. Producer back-pressure spin-waits when the ring buffer reaches a 1024-slot delta, preventing unbounded memory growth.

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
