#pragma once
// ============================================================================
// simd_engine.hpp — AVX2/FMA3 + NEON Vectorized Risk Compute Kernel
//
// The inner loop of the OptiRisk risk engine. Processes ShockPayloads by
// applying fractional deltas to all 500 nodes' portfolio exposures using
// SIMD intrinsics — 4 doubles per clock cycle.
//
// Architecture:
//   x86-64:  AVX2 + FMA3 intrinsics (__m256d, _mm256_fmadd_pd)
//   ARM64:   NEON double-precision  (float64x2_t, vfmaq_f64)
//
// The kernel has 3 phases per shock:
//   Phase 1: SIMD vectorized exposure update (4 nodes/cycle × 5 asset classes)
//   Phase 2: SIMD vectorized NAV recomputation (total_assets − liabilities)
//   Phase 3: Scalar cascade detection + BFS propagation over CSR edges
//
// Memory access pattern:
//   Phase 1–2 are pure sequential SoA sweeps → hardware prefetcher saturates
//   the memory bus. Phase 3 is random-access (graph traversal) — we use
//   explicit __builtin_prefetch to pull edge data into L1 ahead of use.
//
// Throughput target: 500 nodes × 5 fields × 8 bytes = 20 KB of writes.
//   At 256 bits/cycle (AVX2), that's ~80 cycles for the exposure update.
//   The bottleneck is Phase 3 (cascade), not the SIMD math.
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <algorithm>

#include "memory/csr_graph.hpp"
#include "network/wire_protocol.hpp"

// ── Platform SIMD Headers ─────────────────────────────────────────
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>   // AVX2 + FMA3
    #define OPTIRISK_AVX2 1
#elif defined(__aarch64__)
    #include <arm_neon.h>    // NEON double-precision
    #define OPTIRISK_NEON 1
#else
    #define OPTIRISK_SCALAR 1
#endif

// ── Prefetch Toggle (benchmark ablation only) ─────────────────────
//
// Default builds expand to exactly the original __builtin_prefetch calls,
// so the shipped binary is unchanged. Define OPTIRISK_NO_PREFETCH to
// compile every explicit prefetch out; bench/ builds both variants to
// measure what the prefetching is actually worth. Nothing else differs.
//
#ifdef OPTIRISK_NO_PREFETCH
    #define OPTIRISK_PREFETCH(addr, rw, locality) ((void)0)
#else
    #define OPTIRISK_PREFETCH(addr, rw, locality) __builtin_prefetch((addr), (rw), (locality))
#endif

namespace optirisk::compute {

// Using native memory and network models
// Scoped explicitly in the implementation to avoid header pollution

// ── Compile-Time Constants ────────────────────────────────────────
inline constexpr uint32_t SIMD_WIDTH    = 4;       // 4 doubles per vector (AVX2)
inline constexpr float    DEFAULT_THRESH = 0.65f;  // Risk threshold for default
inline constexpr float    CASCADE_FACTOR = 0.6f;   // Contagion attenuation
inline constexpr float    RISK_GAIN      = 1.0f;   // 1.0 = NAV-decline-percent feeds risk 1:1
inline constexpr float    STRESS_THRESH  = 0.40f;  // Above this, neighbors absorb stress
inline constexpr float    STRESS_FACTOR  = 0.15f;  // How much risk a stressed (non-defaulted) node leaks
inline constexpr float    QUIESCENCE_EPS = 1e-5f;  // Risk movement below this counts as "no change"

// ── Hardware Timestamp Counter ───────────────────────────────────
//
// Reads a monotonic hardware counter with zero OS involvement — no
// syscall, no vDSO, no jitter from clock_gettime(). This is the right
// primitive for sub-microsecond latency measurement.
//
// IMPORTANT: on NEITHER platform does this counter tick at the core
// clock, so a raw delta is a TICK count, not a CYCLE count:
//   x86-64: RDTSC returns the invariant TSC, which ticks at a fixed
//           reference frequency (typically the part's base clock) and
//           deliberately does NOT track turbo or frequency scaling.
//   ARM64:  CNTVCT_EL0 is a fixed-rate system counter. Its frequency is
//           architecturally reported by CNTFRQ_EL0 — 1 GHz on Apple
//           Silicon, 24 MHz on many other ARM parts. It is entirely
//           unrelated to core frequency.
//
// Never divide a delta by "the CPU's GHz" to get a duration. Convert
// with ticks_to_ns(), which resolves the real rate at startup.
//
__attribute__((always_inline))
inline uint64_t read_timestamp() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t val;
    asm volatile("mrs %0, CNTVCT_EL0" : "=r"(val));
    return val;
#else
    return 0;  // No hardware counter available
#endif
}

// ── Tick → Nanosecond Conversion ─────────────────────────────────
//
// Resolved exactly once at startup (cold path) by
// calibrate_timestamp_clock(). On ARM64 the counter frequency is
// architecturally enumerable via CNTFRQ_EL0, so the conversion is exact.
// On x86-64 the invariant-TSC rate is not enumerable, so we calibrate
// against steady_clock for a few milliseconds during boot.
//
inline double g_ns_per_tick = 1.0;

inline void calibrate_timestamp_clock() noexcept {
#if defined(__aarch64__)
    uint64_t freq_hz;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq_hz));
    g_ns_per_tick = (freq_hz > 0) ? (1e9 / static_cast<double>(freq_hz)) : 1.0;
#elif defined(__x86_64__) || defined(_M_X64)
    using clock = std::chrono::steady_clock;
    const auto     wall_start = clock::now();
    const uint64_t tick_start = read_timestamp();
    while (clock::now() - wall_start < std::chrono::milliseconds(5)) { }
    const uint64_t tick_end   = read_timestamp();
    const auto     wall_end   = clock::now();

    const double   elapsed_ns =
        std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    const uint64_t ticks = tick_end - tick_start;
    g_ns_per_tick = (ticks > 0) ? (elapsed_ns / static_cast<double>(ticks)) : 1.0;
#else
    g_ns_per_tick = 1.0;  // No counter; ticks_to_ns() is a no-op on zeros
#endif
}

__attribute__((always_inline))
inline uint64_t ticks_to_ns(uint64_t ticks) noexcept {
    return static_cast<uint64_t>(static_cast<double>(ticks) * g_ns_per_tick);
}

// ============================================================================
// Phase 1: SIMD Exposure Update
// ============================================================================
//
// For each asset class, applies: exposure[i] *= (1.0 + delta)
//
// This is equivalent to: exposure[i] = exposure[i] + exposure[i] * delta
// Which maps perfectly to FMA: fmadd(exposure, delta_vec, exposure)
//   → exposure = exposure * delta_vec + exposure
//   → exposure = exposure * (1 + delta)      ... when delta_vec is just delta
//
// Actually, the cleaner FMA form is:
//   result = fmadd(exposure, delta_broadcast, exposure)
//   where delta_broadcast = _mm256_set1_pd(delta)
//   result = exposure * delta + exposure = exposure * (1 + delta)  ✓
//
// Wait — fmadd(a,b,c) = a*b + c. So:
//   fmadd(exposure, delta, exposure) = exposure*delta + exposure
//                                    = exposure * (1 + delta)  ✓  CORRECT.
//

// ── Apply a single delta to one SoA exposure array (4 nodes at a time) ──
//
// This is the innermost hot function. It MUST be inlined — the overhead
// of a function call would dominate on a 500-element array.
//
__attribute__((always_inline))
inline void apply_delta_to_array(double* __restrict__ arr,
                                 const double delta,
                                 const uint32_t count) noexcept {
    // If delta is zero, skip entirely (branch-free hot path for
    // shocks that only affect 1-2 asset classes)
    if (delta == 0.0) [[unlikely]] return;

    const uint32_t vec_end = count & ~(SIMD_WIDTH - 1);  // Round down to multiple of 4

#if defined(OPTIRISK_AVX2)
    // ── AVX2 + FMA3 Path ──────────────────────────────────────────
    //
    // _mm256_set1_pd: broadcast delta to all 4 lanes     [δ,δ,δ,δ]
    // _mm256_loadu_pd: aligned load of 4 doubles          [e0,e1,e2,e3]
    // _mm256_fmadd_pd(a,b,c) = a*b + c
    //   = [e0,e1,e2,e3] * [δ,δ,δ,δ] + [e0,e1,e2,e3]
    //   = [e0*(1+δ), e1*(1+δ), e2*(1+δ), e3*(1+δ)]
    //
    // Throughput: 1 FMA per cycle on Haswell+, 2 per cycle on Skylake+.
    // Latency: 4 cycles (pipelined, so sustained throughput = 1/cycle).
    //
    const __m256d vdelta = _mm256_set1_pd(delta);

    for (uint32_t i = 0; i < vec_end; i += SIMD_WIDTH) {
        // Prefetch 4 cache lines ahead (256 bytes = 32 doubles ahead)
        // This hides the L1 miss latency by starting the memory fetch
        // before we need the data.
        OPTIRISK_PREFETCH(arr + i + 32, 1, 3);  // write intent, L1 temporal

        __m256d exposure = _mm256_loadu_pd(arr + i);
        __m256d result   = _mm256_fmadd_pd(exposure, vdelta, exposure);
        _mm256_storeu_pd(arr + i, result);
    }

#elif defined(OPTIRISK_NEON)
    // ── ARM NEON Path (Apple Silicon / AWS Graviton) ──────────────
    //
    // NEON double-precision is 128-bit (2 doubles per vector), so we
    // process 4 doubles per iteration by using 2 NEON ops.
    //
    // vfmaq_f64(c, a, b) = a*b + c  (same as x86 FMA)
    // vld1q_f64: load 2 doubles
    // vst1q_f64: store 2 doubles
    //
    const float64x2_t vdelta = vdupq_n_f64(delta);

    for (uint32_t i = 0; i < vec_end; i += SIMD_WIDTH) {
        OPTIRISK_PREFETCH(arr + i + 32, 1, 3);

        // Process 4 doubles as 2 × NEON 128-bit ops
        float64x2_t exp_lo = vld1q_f64(arr + i);
        float64x2_t exp_hi = vld1q_f64(arr + i + 2);

        float64x2_t res_lo = vfmaq_f64(exp_lo, exp_lo, vdelta);
        float64x2_t res_hi = vfmaq_f64(exp_hi, exp_hi, vdelta);

        vst1q_f64(arr + i,     res_lo);
        vst1q_f64(arr + i + 2, res_hi);
    }

#else
    // ── Scalar Fallback ───────────────────────────────────────────
    #pragma GCC unroll 4
    for (uint32_t i = 0; i < vec_end; i += SIMD_WIDTH) {
        arr[i]     *= (1.0 + delta);
        arr[i + 1] *= (1.0 + delta);
        arr[i + 2] *= (1.0 + delta);
        arr[i + 3] *= (1.0 + delta);
    }
#endif

    // ── Scalar Tail (handles count % 4 remainder) ─────────────────
    for (uint32_t i = vec_end; i < count; ++i) {
        arr[i] *= (1.0 + delta);
    }
}

// ============================================================================
// Phase 2: SIMD NAV Recomputation
// ============================================================================
//
// NAV[i] = (eq[i] + re[i] + cr[i] + tr[i] + cb[i]) − liabilities[i]
//
// Strategy: 5 aligned loads + 4 additions + 1 subtraction per 4-node vector.
// We also update total_assets[i] as the pre-liability sum.
//

__attribute__((always_inline))
inline void recompute_nav_simd(optirisk::memory::NodeData& nodes, const uint32_t count) noexcept {
    const uint32_t vec_end = count & ~(SIMD_WIDTH - 1);

#if defined(OPTIRISK_AVX2)
    for (uint32_t i = 0; i < vec_end; i += SIMD_WIDTH) {
        // Prefetch next iteration's data into L1
        OPTIRISK_PREFETCH(&nodes.equities_exposure[i + 32],   0, 3);
        OPTIRISK_PREFETCH(&nodes.real_estate_exposure[i + 32], 0, 3);

        // Load all 5 exposure arrays for 4 nodes each
        __m256d eq = _mm256_loadu_pd(&nodes.equities_exposure[i]);
        __m256d re = _mm256_loadu_pd(&nodes.real_estate_exposure[i]);
        __m256d cr = _mm256_loadu_pd(&nodes.crypto_exposure[i]);
        __m256d tr = _mm256_loadu_pd(&nodes.treasuries_exposure[i]);
        __m256d cb = _mm256_loadu_pd(&nodes.corp_bonds_exposure[i]);

        // total = eq + re + cr + tr + cb
        //
        // Chain adds to maximize ILP (instruction-level parallelism):
        //   sum_01 = eq + re        (no dependency)
        //   sum_23 = cr + tr        (no dependency, can execute in parallel)
        //   sum_04 = sum_01 + cb    (depends on sum_01)
        //   total  = sum_04 + sum_23 (depends on both)
        //
        // This gives a 2-cycle critical path instead of 4 if we chained
        // linearly: eq + re + cr + tr + cb.
        //
        __m256d sum_01 = _mm256_add_pd(eq, re);
        __m256d sum_23 = _mm256_add_pd(cr, tr);
        __m256d sum_04 = _mm256_add_pd(sum_01, cb);
        __m256d total  = _mm256_add_pd(sum_04, sum_23);

        // Store total_assets
        _mm256_storeu_pd(&nodes.total_assets[i], total);

        // NAV = total_assets − liabilities
        __m256d liab = _mm256_loadu_pd(&nodes.liabilities[i]);
        __m256d nav  = _mm256_sub_pd(total, liab);
        _mm256_storeu_pd(&nodes.nav[i], nav);
    }

#elif defined(OPTIRISK_NEON)
    for (uint32_t i = 0; i < vec_end; i += SIMD_WIDTH) {
        OPTIRISK_PREFETCH(&nodes.equities_exposure[i + 32],   0, 3);
        OPTIRISK_PREFETCH(&nodes.real_estate_exposure[i + 32], 0, 3);

        // Process as 2 × float64x2_t per array (= 4 doubles)
        for (uint32_t lane = 0; lane < SIMD_WIDTH; lane += 2) {
            float64x2_t eq = vld1q_f64(&nodes.equities_exposure[i + lane]);
            float64x2_t re = vld1q_f64(&nodes.real_estate_exposure[i + lane]);
            float64x2_t cr = vld1q_f64(&nodes.crypto_exposure[i + lane]);
            float64x2_t tr = vld1q_f64(&nodes.treasuries_exposure[i + lane]);
            float64x2_t cb = vld1q_f64(&nodes.corp_bonds_exposure[i + lane]);

            // ILP-optimized addition tree
            float64x2_t sum_01 = vaddq_f64(eq, re);
            float64x2_t sum_23 = vaddq_f64(cr, tr);
            float64x2_t sum_04 = vaddq_f64(sum_01, cb);
            float64x2_t total  = vaddq_f64(sum_04, sum_23);

            vst1q_f64(&nodes.total_assets[i + lane], total);

            float64x2_t liab = vld1q_f64(&nodes.liabilities[i + lane]);
            float64x2_t nav  = vsubq_f64(total, liab);
            vst1q_f64(&nodes.nav[i + lane], nav);
        }
    }

#else
    for (uint32_t i = 0; i < vec_end; i += SIMD_WIDTH) {
        for (uint32_t j = 0; j < SIMD_WIDTH; ++j) {
            double total = nodes.equities_exposure[i+j]
                         + nodes.real_estate_exposure[i+j]
                         + nodes.crypto_exposure[i+j]
                         + nodes.treasuries_exposure[i+j]
                         + nodes.corp_bonds_exposure[i+j];
            nodes.total_assets[i+j] = total;
            nodes.nav[i+j] = total - nodes.liabilities[i+j];
        }
    }
#endif

    // Scalar tail
    for (uint32_t i = vec_end; i < count; ++i) {
        double total = nodes.equities_exposure[i]
                     + nodes.real_estate_exposure[i]
                     + nodes.crypto_exposure[i]
                     + nodes.treasuries_exposure[i]
                     + nodes.corp_bonds_exposure[i];
        nodes.total_assets[i] = total;
        nodes.nav[i] = total - nodes.liabilities[i];
    }
}

// ============================================================================
// Phase 3: Cascade Detection + CSR BFS Propagation (Scalar)
// ============================================================================
//
// After the SIMD exposure update + NAV recompute, scan for nodes whose
// risk score exceeds the default threshold. For each newly-defaulted node,
// propagate contagion to all CSR neighbors weighted by debt/total_assets.
//
// This phase is inherently scalar — graph traversal is random-access and
// branch-heavy. We compensate with aggressive prefetching.
//
// The risk_score update is a simplified model:
//   risk_delta = max(0, -delta_nav / old_nav) * 0.1
//   Clamped to [0.0, 1.0].
//
// Returns the number of nodes that defaulted this tick (for diagnostics).
//

struct CascadeResult {
    uint32_t defaults_triggered;  // Nodes that crossed the default threshold this call
    uint32_t edges_propagated;    // Total contagion edges fired
    uint32_t risk_movements;      // Nodes whose risk score changed at all this call
};

__attribute__((always_inline))
inline CascadeResult run_cascade(optirisk::memory::CSRGraph& graph,
                                 const double* __restrict__ old_nav,
                                 const uint32_t count) noexcept {
    CascadeResult result{0, 0, 0};

    for (uint32_t nid = 0; nid < count; ++nid) {
        // Skip already-defaulted nodes — they neither absorb new risk nor
        // re-fire contagion (that's handled at first-default).
        if (graph.nodes.is_defaulted[nid] != 0) continue;

        // ── Risk Score Update ─────────────────────────────────────
        // Based on NAV decline relative to PRE-SHOCK NAV. Using the
        // pre-shock baseline (rather than the previous tick) means a
        // sustained 30% NAV drop continues to apply pressure round
        // after round, rather than the system going quiescent the
        // moment prices stop moving.
        const double prev = old_nav[nid];
        const double curr = graph.nodes.nav[nid];

        if (prev > 1.0) [[likely]] {
            const double decline = prev - curr;
            if (decline > 0.0) {
                const float risk_delta = static_cast<float>(decline / prev) * RISK_GAIN;
                const float old_risk   = graph.nodes.risk_score[nid];
                const float new_risk   = std::clamp(old_risk + risk_delta, 0.0f, 1.0f);
                graph.nodes.risk_score[nid] = new_risk;
                if (std::fabs(new_risk - old_risk) > QUIESCENCE_EPS) ++result.risk_movements;
            }
        }

        const float risk = graph.nodes.risk_score[nid];

        // ── Default Detection ─────────────────────────────────────
        if ((risk > DEFAULT_THRESH || curr < 0.0)) [[unlikely]] {
            graph.nodes.is_defaulted[nid] = 1;
            graph.nodes.risk_score[nid] = 1.0f;
            ++result.defaults_triggered;

            // ABLATION FLAG, measurement only. OPTIRISK_NO_COUNTERPARTY_CONTAGION
            // removes propagation along CSR edges. The node still defaults on
            // its own NAV; it just stops pushing risk onto its counterparties.
#ifndef OPTIRISK_NO_COUNTERPARTY_CONTAGION
            const auto [begin, end] = graph.neighbors(nid);
            if (begin < end) {
                OPTIRISK_PREFETCH(&graph.edges.col_idx[begin], 0, 3);
                OPTIRISK_PREFETCH(&graph.edges.weight[begin],  0, 3);
            }

            for (uint32_t e = begin; e < end; ++e) {
                const uint32_t neighbor = graph.edges.col_idx[e];
                if (graph.nodes.is_defaulted[neighbor] != 0) continue;
                const double debt = graph.edges.weight[e];
                const double neighbor_assets = graph.nodes.total_assets[neighbor] + 1.0;
                const float contagion = static_cast<float>(debt / neighbor_assets) * CASCADE_FACTOR;
                const float old_n = graph.nodes.risk_score[neighbor];
                const float new_n = std::clamp(old_n + contagion, 0.0f, 1.0f);
                graph.nodes.risk_score[neighbor] = new_n;
                if (std::fabs(new_n - old_n) > QUIESCENCE_EPS) ++result.risk_movements;
                ++result.edges_propagated;
            }
#endif
        }
        // ── Stress Contagion ─────────────────────────────────────
        // Non-defaulted but heavily stressed nodes still leak risk to
        // their counterparties, just at a reduced rate. This is what
        // keeps the cascade ticking past the initial shock — a -30%
        // hit may not break any firm in round 0, but the resulting
        // ~0.5 risk scores propagate over a few rounds and eventually
        // push the most-leveraged firms over the line.
        // Same flag: stress contagion is the other edge-propagation path.
#ifdef OPTIRISK_NO_COUNTERPARTY_CONTAGION
        else if (false) {
#else
        else if (risk > STRESS_THRESH) {
#endif
            const auto [begin, end] = graph.neighbors(nid);
            for (uint32_t e = begin; e < end; ++e) {
                const uint32_t neighbor = graph.edges.col_idx[e];
                if (graph.nodes.is_defaulted[neighbor] != 0) continue;
                const double debt = graph.edges.weight[e];
                const double neighbor_assets = graph.nodes.total_assets[neighbor] + 1.0;
                const float leak = static_cast<float>(debt / neighbor_assets)
                                   * STRESS_FACTOR * (risk - STRESS_THRESH);
                if (leak <= QUIESCENCE_EPS) continue;
                const float old_n = graph.nodes.risk_score[neighbor];
                const float new_n = std::clamp(old_n + leak, 0.0f, 1.0f);
                graph.nodes.risk_score[neighbor] = new_n;
                if (std::fabs(new_n - old_n) > QUIESCENCE_EPS) {
                    ++result.risk_movements;
                    ++result.edges_propagated;
                }
            }
        }
    }

    return result;
}

// ============================================================================
// Top-Level Tick Function: apply_shock_simd()
// ============================================================================
//
// The single entry point called by the Compute thread per ShockPayload.
//
// Pipeline:
//   1. Snapshot old NAV (for risk delta calculation)
//   2. SIMD Phase 1: Apply fractional deltas to all 5 exposure arrays
//   3. SIMD Phase 2: Recompute total_assets and NAV
//   4. Scalar Phase 3: Cascade detection + BFS propagation
//
// This function is NOT thread-safe — it is called exclusively by the
// single Compute thread. No atomics, no locks.
//

struct TickResult {
    CascadeResult cascade;
    uint32_t      nodes_processed;
    uint64_t      compute_ns;  // Wall-clock nanoseconds for the entire SIMD tick
};

inline TickResult apply_shock_simd(optirisk::memory::CSRGraph& graph,
                                   const optirisk::network::ShockPayload& shock) noexcept {
    const uint32_t N = graph.num_nodes;

    // ── TELEMETRY: capture hardware counter BEFORE any work ────────
    const uint64_t t0 = read_timestamp();

    // ── Step 0: Snapshot old NAV for risk delta calculation ────────
    // We need the pre-shock NAV to compute how much each node's
    // risk increased. Stack-allocate the snapshot — 500 × 8 = 4 KB.
    alignas(64) double old_nav[optirisk::memory::MAX_NODES];

#if defined(OPTIRISK_AVX2)
    // SIMD copy: 4 doubles per iteration
    for (uint32_t i = 0; i < N; i += SIMD_WIDTH) {
        __m256d v = _mm256_loadu_pd(&graph.nodes.nav[i]);
        _mm256_storeu_pd(&old_nav[i], v);
    }
#elif defined(OPTIRISK_NEON)
    for (uint32_t i = 0; i < N; i += SIMD_WIDTH) {
        float64x2_t lo = vld1q_f64(&graph.nodes.nav[i]);
        float64x2_t hi = vld1q_f64(&graph.nodes.nav[i + 2]);
        vst1q_f64(&old_nav[i],     lo);
        vst1q_f64(&old_nav[i + 2], hi);
    }
#else
    std::memcpy(old_nav, graph.nodes.nav.data(), N * sizeof(double));
#endif

    // ── Phase 1: SIMD Exposure Update ─────────────────────────────
    // Each call processes all N nodes with SIMD, applying
    // exposure[i] = exposure[i] * (1 + delta) via FMA.
    //
    // Throughput: 5 arrays × 500 nodes × 8 bytes = 20 KB
    // At 32 bytes/cycle (AVX2 store BW): ~625 cycles total.
    //
    apply_delta_to_array(graph.nodes.equities_exposure.data(),
                         shock.equities_delta, N);
    apply_delta_to_array(graph.nodes.real_estate_exposure.data(),
                         shock.real_estate_delta, N);
    apply_delta_to_array(graph.nodes.crypto_exposure.data(),
                         shock.crypto_delta, N);
    apply_delta_to_array(graph.nodes.treasuries_exposure.data(),
                         shock.treasuries_delta, N);
    apply_delta_to_array(graph.nodes.corp_bonds_exposure.data(),
                         shock.corp_bonds_delta, N);

    // ── Phase 2: SIMD NAV Recomputation ───────────────────────────
    // total_assets[i] = sum of 5 exposures
    // nav[i] = total_assets[i] − liabilities[i]
    //
    // Uses ILP-optimized addition tree to minimize critical path.
    //
    recompute_nav_simd(graph.nodes, N);

    // ── Phase 3: Cascade Detection + BFS Propagation ──────────────
    CascadeResult cascade = run_cascade(graph, old_nav, N);

    // ── TELEMETRY: capture hardware counter AFTER all work ──────────
    const uint64_t t1 = read_timestamp();

    return TickResult{cascade, N, ticks_to_ns(t1 - t0)};
}

} // namespace optirisk::compute
