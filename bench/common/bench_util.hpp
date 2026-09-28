#pragma once
// ============================================================================
// bench_util.hpp — measurement primitives shared by every bench/ binary.
//
// Rules this file exists to enforce:
//   1. No allocation inside a timed region. Sample storage is reserved up
//      front; percentiles are computed after the run.
//   2. Durations come from the project's own calibrated counter
//      (read_timestamp + ticks_to_ns), never from an assumed clock rate.
//   3. Every result carries its conditions: sample count, whether thread
//      pinning actually succeeded, and the build variant.
//
// This file is measurement-only. It is never included by src/.
// ============================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "compute/simd_engine.hpp"  // read_timestamp, ticks_to_ns, calibrate

#if defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

namespace optirisk::bench {

using optirisk::compute::read_timestamp;
using optirisk::compute::ticks_to_ns;

// ── Build variant label ────────────────────────────────────────────
// Stamped into every report so a results file can never be mistaken for
// one produced by the other build.
inline const char* prefetch_variant() noexcept {
#ifdef OPTIRISK_NO_PREFETCH
    return "prefetch=OFF";
#else
    return "prefetch=ON";
#endif
}

// simd_engine.hpp's path. NOT the same as the option kernel's path:
// black_scholes_simd.hpp has no NEON branch at all, so on ARM it runs plain
// scalar C++ while simd_engine still reports NEON. Mixing the two labels is
// how an "AVX2 kernel" number ends up describing a scalar loop.
inline const char* bs_kernel_variant() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return "AVX2-8lane-f32";
#else
    return "SCALAR-FALLBACK-f32 (no NEON path exists in black_scholes_simd.hpp)";
#endif
}

inline const char* simd_variant() noexcept {
#if defined(OPTIRISK_AVX2)
    return "AVX2";
#elif defined(OPTIRISK_NEON)
    return "NEON";
#else
    return "scalar";
#endif
}

// ── Thread pinning ─────────────────────────────────────────────────
//
// Returns true ONLY if the thread was actually pinned. macOS has no
// usable affinity API for this, so it returns false there and the caller
// is expected to say so in its output rather than silently pretending.
//
inline bool pin_to_core(int core_id) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core_id;
    return false;
#endif
}

// ── Core selection ─────────────────────────────────────────────────
//
// Hardcoding core ids breaks on small machines: a 2-vCPU CI runner has only
// cores 0 and 1, so pinning to core 4 silently fails and the run quietly
// loses its isolation. Pick from what the machine actually has, and let the
// environment override for a box with a known isolated set.
//
inline int bench_core(int which) noexcept {
    const char* env = std::getenv(which == 0 ? "OPTIRISK_BENCH_CORE_A"
                                             : "OPTIRISK_BENCH_CORE_B");
    if (env != nullptr) {
        const int v = std::atoi(env);
        if (v >= 0) return v;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw >= 6) return (which == 0) ? 2 : 4;   // leave 0/1 to the OS
    if (hw >= 2) return (which == 0) ? 0 : 1;
    return 0;
}

// ── Percentile summary ─────────────────────────────────────────────
struct Stats {
    std::size_t count = 0;
    double mean_ns = 0.0;
    uint64_t min_ns = 0;
    uint64_t p50_ns = 0;
    uint64_t p90_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
};

// Nearest-rank percentile on an already-sorted span: the smallest value
// at or above the given fraction of the distribution. No interpolation —
// every reported number is a sample that actually occurred.
inline uint64_t nearest_rank(const std::vector<uint64_t>& sorted, double q) noexcept {
    if (sorted.empty()) return 0;
    std::size_t rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size())));
    if (rank == 0) rank = 1;
    if (rank > sorted.size()) rank = sorted.size();
    return sorted[rank - 1];
}

// Consumes by value: sorting is destructive and must not touch the
// caller's raw samples, which get written out verbatim.
inline Stats summarize(std::vector<uint64_t> samples) {
    Stats s{};
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());

    long double acc = 0.0L;
    for (uint64_t v : samples) acc += static_cast<long double>(v);

    s.count   = samples.size();
    s.mean_ns = static_cast<double>(acc / static_cast<long double>(samples.size()));
    s.min_ns  = samples.front();
    s.p50_ns  = nearest_rank(samples, 0.50);
    s.p90_ns  = nearest_rank(samples, 0.90);
    s.p99_ns  = nearest_rank(samples, 0.99);
    s.p999_ns = nearest_rank(samples, 0.999);
    s.max_ns  = samples.back();
    return s;
}

// ── Effective timer granularity ────────────────────────────────────
//
// The calibrated ns/tick figure is the counter's NOMINAL rate, which is not
// the same as the smallest interval it can actually resolve. On Apple Silicon
// CNTVCT_EL0 enumerates as 1 GHz but advances in jumps, so anything shorter
// than one jump reads as zero. Measuring the smallest nonzero back-to-back
// delta puts the real floor in every log, where it cannot be overlooked when
// a sub-microsecond number is interpreted.
//
inline uint64_t measure_timer_granularity_ns() noexcept {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 20000; ++i) {
        const uint64_t a = read_timestamp();
        const uint64_t b = read_timestamp();
        const uint64_t d = (b > a) ? (b - a) : 0;
        if (d > 0 && d < best) best = d;
    }
    return (best == UINT64_MAX) ? 0 : ticks_to_ns(best);
}

// ── Reporting ──────────────────────────────────────────────────────
inline void print_header(const char* bench_name) {
    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════════════════════\n");
    std::printf("  %s\n", bench_name);
    std::printf("  build: %s | %s\n", simd_variant(), prefetch_variant());
    std::printf("  tick rate: %.6f ns/tick (%.3f MHz counter)\n",
                optirisk::compute::g_ns_per_tick,
                1000.0 / optirisk::compute::g_ns_per_tick);
    const uint64_t gran = measure_timer_granularity_ns();
    std::printf("  effective timer granularity: %llu ns (smallest nonzero back-to-back delta)\n",
                static_cast<unsigned long long>(gran));
    if (gran > 20) {
        std::printf("  *** measurements below ~%llu ns are QUANTIZED to this floor — "
                    "do not report them ***\n",
                    static_cast<unsigned long long>(gran * 4));
    }
    std::printf("═══════════════════════════════════════════════════════════════════════\n");
}

inline void print_stats(const char* label, const Stats& s, const char* conditions) {
    std::printf("\n  %s\n", label);
    std::printf("    conditions : %s\n", conditions);
    std::printf("    samples    : %zu\n", s.count);
    if (s.count == 0) {
        std::printf("    (no samples collected)\n");
        return;
    }
    std::printf("    mean       : %10.1f ns\n", s.mean_ns);
    std::printf("    min        : %10llu ns\n", static_cast<unsigned long long>(s.min_ns));
    std::printf("    p50        : %10llu ns\n", static_cast<unsigned long long>(s.p50_ns));
    std::printf("    p90        : %10llu ns\n", static_cast<unsigned long long>(s.p90_ns));
    std::printf("    p99        : %10llu ns\n", static_cast<unsigned long long>(s.p99_ns));
    std::printf("    p99.9      : %10llu ns\n", static_cast<unsigned long long>(s.p999_ns));
    std::printf("    max        : %10llu ns\n", static_cast<unsigned long long>(s.max_ns));
}

inline void print_throughput(const char* label, uint64_t events, uint64_t elapsed_ns,
                             const char* conditions) {
    const double secs = static_cast<double>(elapsed_ns) / 1e9;
    const double rate = (secs > 0.0) ? static_cast<double>(events) / secs : 0.0;
    std::printf("\n  %s\n", label);
    std::printf("    conditions : %s\n", conditions);
    std::printf("    events     : %llu in %.6f s\n",
                static_cast<unsigned long long>(events), secs);
    std::printf("    throughput : %.0f events/s (%.2f M/s)\n", rate, rate / 1e6);
}

// Emit one machine-readable line per metric so run_all.sh can assemble the
// results table without re-parsing the human-readable block above.
inline void emit_csv_stats(const char* bench, const char* metric,
                           const Stats& s, const char* conditions) {
    std::printf("CSV,%s,%s,%s,%s,%zu,%.1f,%llu,%llu,%llu,%llu,%llu,%llu,\"%s\"\n",
                bench, metric, simd_variant(), prefetch_variant(), s.count, s.mean_ns,
                static_cast<unsigned long long>(s.min_ns),
                static_cast<unsigned long long>(s.p50_ns),
                static_cast<unsigned long long>(s.p90_ns),
                static_cast<unsigned long long>(s.p99_ns),
                static_cast<unsigned long long>(s.p999_ns),
                static_cast<unsigned long long>(s.max_ns),
                conditions);
}

inline void emit_csv_scalar(const char* bench, const char* metric,
                            double value, const char* unit, const char* conditions) {
    std::printf("SCALAR,%s,%s,%s,%s,%.6g,%s,\"%s\"\n",
                bench, metric, simd_variant(), prefetch_variant(),
                value, unit, conditions);
}

// ── Busy-wait pacing ───────────────────────────────────────────────
//
// Spins on the hardware counter until `target_ns` have elapsed since
// `start_ticks`. Used to hold the producer at a fixed arrival rate so
// the ring stays at queue depth ~1 — otherwise a flat-out producer
// measures queueing delay, not handoff latency.
//
inline void spin_until_ns(uint64_t start_ticks, uint64_t target_ns) noexcept {
    if (target_ns == 0) return;
    while (ticks_to_ns(read_timestamp() - start_ticks) < target_ns) {
#if defined(__x86_64__) || defined(_M_X64)
        asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
    }
}

// ── Raw sample dump ────────────────────────────────────────────────
// Percentiles are a summary; the raw distribution is the evidence.
inline void dump_samples(const std::string& path, const std::vector<uint64_t>& samples) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "  [warn] could not open %s for raw sample dump\n", path.c_str());
        return;
    }
    std::fprintf(f, "latency_ns\n");
    for (uint64_t v : samples) {
        std::fprintf(f, "%llu\n", static_cast<unsigned long long>(v));
    }
    std::fclose(f);
    std::printf("    raw        : %s (%zu samples)\n", path.c_str(), samples.size());
}

// Keeps the optimizer from deleting work whose result is otherwise unused.
template <typename T>
inline void keep(T const& value) noexcept {
    asm volatile("" : : "r,m"(value) : "memory");
}

}  // namespace optirisk::bench
