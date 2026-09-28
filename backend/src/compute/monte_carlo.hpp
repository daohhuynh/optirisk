#pragma once
// ============================================================================
// monte_carlo.hpp — Monte Carlo VaR Simulator
//
// Runs stochastic paths to calculate 95th percentile Value at Risk (VaR)
// for all nodes. Uses Welford's online algorithm to avoid storing millions
// of path outcomes, ensuring zero allocation and L1 cache fit.
// ============================================================================

#include <cstdint>
#include <array>
#include <cmath>

#include "memory/csr_graph.hpp"
#include "network/wire_protocol.hpp"
#include "compute/simd_engine.hpp"

namespace optirisk::compute {

inline constexpr uint32_t MC_PATHS = 1024;

// ── Lightweight PRNG (xoshiro256**) ───────────────────────────────────────
struct fast_prng {
    uint64_t s[4];

    __attribute__((always_inline))
    static inline uint64_t rotl(const uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }

    __attribute__((always_inline))
    inline uint64_t next() noexcept {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    // Fast approximation of Normal(0, 1) using Irwin-Hall
    __attribute__((always_inline))
    inline double normal() noexcept {
        double sum = 0.0;
        for (int i=0; i<12; ++i) {
            sum += static_cast<double>(next() >> 11) * (1.0 / (1ULL << 53));
        }
        return sum - 6.0;
    }
};

struct alignas(64) VaRResult {
    std::array<double, optirisk::memory::MAX_NODES> var_95;     // 95% worst-case drop
    std::array<double, optirisk::memory::MAX_NODES> expected;   // Mean drop
    uint32_t paths_run;
    uint64_t compute_ns;   // Wall-clock nanoseconds for the full MC burst
};

// ── Monte Carlo Tick ──────────────────────────────────────────────────────
__attribute__((always_inline))
inline VaRResult run_monte_carlo_var(
    const optirisk::memory::CSRGraph& graph,
    const optirisk::network::ShockPayload& base_shock
) noexcept {
    const uint64_t start_ticks = read_timestamp();

    VaRResult result{};
    result.paths_run = MC_PATHS;

    // Welford's variables: per-node mean and M2 (sum of squared differences)
    alignas(64) std::array<double, optirisk::memory::MAX_NODES> mean{};
    alignas(64) std::array<double, optirisk::memory::MAX_NODES> M2{};

    fast_prng rng{ {12345ULL, 67890ULL, 13579ULL, 24680ULL} };

    // We only need the pre-shock exposures and liabilities to do a stateless calc
    const auto& nodes = graph.nodes;
    const uint32_t count = graph.num_nodes;
    [[maybe_unused]] const uint32_t vec_end = count & ~(SIMD_WIDTH - 1);

    for (uint32_t path = 1; path <= MC_PATHS; ++path) {
        // Jitter the shock randomly. Volatility ~ 15% relative.
        double jitter = 1.0 + (rng.normal() * 0.15);
        
        optirisk::network::ShockPayload path_shock = base_shock;
        path_shock.equities_delta    *= jitter;
        path_shock.real_estate_delta *= jitter;
        path_shock.crypto_delta      *= jitter;
        path_shock.treasuries_delta  *= jitter;
        path_shock.corp_bonds_delta  *= jitter;

        // Extract deltas eagerly for vectorized loop
        const double eq_d = 1.0 + path_shock.equities_delta;
        const double re_d = 1.0 + path_shock.real_estate_delta;
        const double cr_d = 1.0 + path_shock.crypto_delta;
        const double tr_d = 1.0 + path_shock.treasuries_delta;
        const double cb_d = 1.0 + path_shock.corp_bonds_delta;

#if defined(OPTIRISK_AVX2)
        const __m256d v_eq_d = _mm256_set1_pd(eq_d);
        const __m256d v_re_d = _mm256_set1_pd(re_d);
        const __m256d v_cr_d = _mm256_set1_pd(cr_d);
        const __m256d v_tr_d = _mm256_set1_pd(tr_d);
        const __m256d v_cb_d = _mm256_set1_pd(cb_d);

        for (uint32_t i = 0; i < vec_end; i += 4) {
            __m256d eq = _mm256_loadu_pd(&nodes.equities_exposure[i]);
            __m256d re = _mm256_loadu_pd(&nodes.real_estate_exposure[i]);
            __m256d cr = _mm256_loadu_pd(&nodes.crypto_exposure[i]);
            __m256d tr = _mm256_loadu_pd(&nodes.treasuries_exposure[i]);
            __m256d cb = _mm256_loadu_pd(&nodes.corp_bonds_exposure[i]);

            __m256d sum_01 = _mm256_add_pd(_mm256_mul_pd(eq, v_eq_d), _mm256_mul_pd(re, v_re_d));
            __m256d sum_23 = _mm256_add_pd(_mm256_mul_pd(cr, v_cr_d), _mm256_mul_pd(tr, v_tr_d));
            __m256d sum_04 = _mm256_add_pd(sum_01, _mm256_mul_pd(cb, v_cb_d));
            __m256d total  = _mm256_add_pd(sum_04, sum_23);

            __m256d liab = _mm256_loadu_pd(&nodes.liabilities[i]);
            __m256d new_nav = _mm256_sub_pd(total, liab);

            __m256d old_nav = _mm256_loadu_pd(&nodes.nav[i]);
            __m256d nav_drop = _mm256_sub_pd(old_nav, new_nav); // Positive = a loss

            alignas(32) double drops[4];
            _mm256_storeu_pd(drops, nav_drop);

            #pragma GCC unroll 4
            for (int k=0; k<4; ++k) {
                double x = drops[k];
                double delta = x - mean[i+k];
                mean[i+k] += delta / path;
                double delta2 = x - mean[i+k];
                M2[i+k] += delta * delta2;
            }
        }
#else
        #pragma GCC unroll 4
        for (uint32_t i = 0; i < count; ++i) {
            double total = (nodes.equities_exposure[i] * eq_d)
                         + (nodes.real_estate_exposure[i] * re_d)
                         + (nodes.crypto_exposure[i] * cr_d)
                         + (nodes.treasuries_exposure[i] * tr_d)
                         + (nodes.corp_bonds_exposure[i] * cb_d);
            
            double new_nav = total - nodes.liabilities[i];
            double nav_drop = nodes.nav[i] - new_nav;

            double delta = nav_drop - mean[i];
            mean[i] += delta / path;
            double delta2 = nav_drop - mean[i];
            M2[i] += delta * delta2;
        }
#endif
    }

    // Compute P95 VaR using analytical normal approximation
    // VaR(95%) = Mean Drop + 1.645 * StdDev
    #pragma GCC unroll 8
    for (uint32_t i = 0; i < count; ++i) {
        double variance = M2[i] / MC_PATHS;
        double std_dev = std::sqrt(variance);
        result.expected[i] = mean[i];
        result.var_95[i] = mean[i] + (1.645 * std_dev);
    }

    const uint64_t end_ticks = read_timestamp();
    result.compute_ns = (end_ticks > start_ticks) ? ticks_to_ns(end_ticks - start_ticks) : 0;

    return result;
}

} // namespace optirisk::compute
