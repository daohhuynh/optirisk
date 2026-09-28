// ============================================================================
// bench_blackscholes.cpp — Step 2.3: option kernel throughput and accuracy.
//
// What the kernel actually computes, and therefore what is measured here:
// compute_options_m2m() produces the option DELTA and a hedge volume. It does
// NOT compute an option price, nor gamma/vega/theta. So "price error" has no
// subject; delta error does, and that is what is reported.
//
// Four measurements:
//   1. Throughput: the shipped kernel vs a double-precision std::erfc
//      reference, options/second, single core.
//   2. Delta accuracy: max and mean absolute error vs that reference over a
//      grid of (S, K, sigma, r, T), broken out by moneyness so the error's
//      structure is visible rather than averaged away.
//   3. Fast-log accuracy: the 2z(1 + z^2/3 + z^4/5 + z^6/7) series vs
//      std::log, as a function of S/K. This is isolated because it is a
//      prime suspect for any moneyness-dependent error found in (2).
//   4. Reciprocal accuracy: _mm256_rcp_ps + one Newton-Raphson step vs a
//      true division. x86-only; skipped and reported as skipped elsewhere.
//
// Counts are kept a multiple of 8 throughout: the AVX2 path processes
// `count & ~7` and its scalar tail is compiled out, so a non-multiple-of-8
// count would silently leave entries unpriced and pollute the error stats.
// ============================================================================

#include <cmath>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/bench_util.hpp"

#include "compute/black_scholes_simd.hpp"
#include "memory/options_book.hpp"

using namespace optirisk::bench;
using optirisk::memory::OptionsBook;

namespace {

constexpr uint32_t BATCH = 496;  // 62 * 8 — exercises the vector path with no tail

// ── Double-precision reference ─────────────────────────────────────
// Textbook Black-Scholes delta. Phi via std::erfc, which is correctly
// rounded to well under a double ulp in libm — the right yardstick.
double ref_delta(double S, double K, double sigma, double r, double T, double type) {
    const double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
    const double phi = 0.5 * std::erfc(-d1 * M_SQRT1_2);
    return (type < 0.0) ? (phi - 1.0) : phi;
}

// Scalar throughput baseline: same math as ref_delta, written as a plain
// loop so the comparison is "shipped kernel vs what you'd write by hand".
void scalar_delta_batch(const float* K, const float* T, const float* sigma,
                        const float* r, const float* type, float S,
                        float* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const double d1 = (std::log(static_cast<double>(S) / K[i])
                           + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i])
                          / (sigma[i] * std::sqrt(static_cast<double>(T[i])));
        const double phi = 0.5 * std::erfc(-d1 * M_SQRT1_2);
        out[i] = static_cast<float>((type[i] < 0.0f) ? (phi - 1.0) : phi);
    }
}

// ── Scalar f32, same algorithm as the AVX2 kernel, one lane ────────
//
// This exists to split the measured speedup into its two causes. The f64
// std::erfc baseline differs from the shipped kernel in TWO ways at once:
// cheaper math (f32 polynomial approximations instead of libm) and eight
// lanes instead of one. Comparing only those two conflates them.
//
// Every operation below mirrors compute_options_m2m exactly: the same fast-log
// series, the same Horner order on the A&S coefficients, the same saturation
// of y at Y_SATURATE, and the same 14-bit reciprocal plus one Newton-Raphson
// step rather than a true division. The only difference is lane count, so
// (this vs AVX2) isolates vectorization and (f64 libm vs this) isolates math.
void scalar_f32_approx_batch(const float* K, const float* T, const float* sigma,
                             const float* r, const float* type, float S,
                             float* out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const float S_over_K = S / K[i];
        const float z  = (S_over_K - 1.0f) / (S_over_K + 1.0f);
        const float z2 = z * z;
        float poly_L = std::fma(z2, 0.142857142f, 0.200000000f);
        poly_L = std::fma(poly_L, z2, 0.333333333f);
        poly_L = std::fma(poly_L, z2, 1.0f);
        const float ln_S_K = 2.0f * z * poly_L;

        const float sigma_sq = sigma[i] * sigma[i];
        const float drift  = std::fma(0.5f, sigma_sq, r[i]);
        const float d1_num = std::fma(drift, T[i], ln_S_K);
        const float d1     = d1_num / (sigma[i] * std::sqrt(T[i]));

        float y = std::fabs(d1) * 0.707106781f;
        y = std::min(y, optirisk::compute::Y_SATURATE);

        float pp = std::fma(0.0000430638f, y, 0.0002765672f);
        pp = std::fma(pp, y, 0.0001520143f);
        pp = std::fma(pp, y, 0.0092705272f);
        pp = std::fma(pp, y, 0.0422820123f);
        pp = std::fma(pp, y, 0.0705230784f);
        pp = std::fma(pp, y, 1.0f);
        pp = pp * pp; pp = pp * pp; pp = pp * pp; pp = pp * pp;  // ^16

#if defined(HAS_AVX2)
        // Scalar form of the kernel's reciprocal: 14-bit estimate, one NR step.
        float inv = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(pp)));
        inv = inv * (2.0f - pp * inv);
#else
        const float inv = 1.0f / pp;   // no rcp instruction here; noted in output
#endif
        const float erfc_v = inv;

        const float half_erfc = 0.5f * erfc_v;
        const float phi = (d1 < 0.0f) ? half_erfc : (1.0f - half_erfc);
        float nd = phi + ((type[i] < 0.0f) ? -1.0f : 0.0f);
        if (type[i] == 0.0f) nd = 0.0f;
        out[i] = nd;
    }
}

// The kernel's fast log, extracted verbatim for isolated error measurement.
double fast_log_series(double x) {
    const double z = (x - 1.0) / (x + 1.0);
    const double z2 = z * z;
    return 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (1.0 / 5.0 + z2 * (1.0 / 7.0))));
}

struct ErrStats {
    double max_abs = 0.0;
    double mean_abs = 0.0;
    std::size_t n = 0;              // finite comparisons only
    std::size_t nan_kernel = 0;     // kernel returned a non-finite delta
    std::size_t nan_ref = 0;        // reference returned a non-finite delta
    double worst_S = 0, worst_K = 0, worst_sig = 0, worst_T = 0;
    double nan_S = 0, nan_K = 0, nan_sig = 0, nan_T = 0;
    bool nan_seen = false;

    // A non-finite result is a correctness failure, not a large error, and
    // averaging it in would poison every other number. Count them separately
    // and record the first input that produced one.
    void add(double got, double want, double S, double K, double sig, double T) {
        if (!std::isfinite(got)) {
            ++nan_kernel;
            if (!nan_seen) { nan_seen = true; nan_S = S; nan_K = K; nan_sig = sig; nan_T = T; }
            return;
        }
        if (!std::isfinite(want)) { ++nan_ref; return; }
        const double a = std::fabs(got - want);
        if (a > max_abs) { max_abs = a; worst_S = S; worst_K = K; worst_sig = sig; worst_T = T; }
        mean_abs += a;
        ++n;
    }
    double mean() const { return (n > 0) ? mean_abs / static_cast<double>(n) : 0.0; }
};

}  // namespace

int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";
    (void)outdir;

    print_header("bench_blackscholes — option kernel throughput and accuracy");
    std::printf("  option kernel path: %s\n", bs_kernel_variant());
    std::printf("  NOTE: this kernel computes DELTA only — no price, no gamma/vega/theta.\n");
    std::printf("  Reference: double-precision Black-Scholes delta via std::erfc.\n");

    static OptionsBook book;
    optirisk::memory::init_options_book(book);

    // Grid. Chosen to span realistic equity-option territory rather than
    // a neighbourhood where any approximation would look good.
    const double SPOTS[]  = {400.0, 450.0, 475.0, 500.0, 525.0, 550.0, 600.0};
    const double STRIKES[] = {350.0, 400.0, 450.0, 475.0, 500.0, 525.0, 550.0, 600.0, 650.0};
    const double VOLS[]   = {0.10, 0.20, 0.40, 0.80};
    const double RATES[]  = {0.00, 0.02, 0.05};
    const double EXPS[]   = {1.0 / 52.0, 1.0 / 12.0, 0.25, 0.5, 1.0, 2.0};
    const double TYPES[]  = {1.0, -1.0};

    // ── 2. Delta accuracy ──────────────────────────────────────────
    ErrStats overall;
    ErrStats atm;    // |ln(S/K)| <= 0.05
    ErrStats near_;  // 0.05 < |ln(S/K)| <= 0.20
    ErrStats far;    // |ln(S/K)| > 0.20

    alignas(64) static std::vector<float> got(BATCH);
    std::vector<double> want(BATCH);
    std::vector<double> gS(BATCH), gK(BATCH), gSig(BATCH), gT(BATCH);

    alignas(64) static std::array<float, optirisk::memory::MAX_NODES> hedge{};

    for (double S : SPOTS) {
        // Fill the book in BATCH-sized chunks over the (K, sigma, r, T, type) grid.
        uint32_t filled = 0;
        auto flush = [&]() {
            if (filled == 0) return;
            // Pad the remainder with a repeat of the last entry so `count`
            // stays a multiple of 8 and no entry goes unpriced.
            const uint32_t count = ((filled + 7) / 8) * 8;
            for (uint32_t i = filled; i < count; ++i) {
                book.strikes[i]  = book.strikes[filled - 1];
                book.expiries[i] = book.expiries[filled - 1];
                book.iv[i]       = book.iv[filled - 1];
                book.rates[i]    = book.rates[filled - 1];
                book.types[i]    = book.types[filled - 1];
                book.positions[i] = 1.0f;
                gS[i] = gS[filled - 1]; gK[i] = gK[filled - 1];
                gSig[i] = gSig[filled - 1]; gT[i] = gT[filled - 1];
                want[i] = want[filled - 1];
            }
            book.last_delta.fill(0.0f);
            optirisk::compute::compute_options_m2m(&book, static_cast<float>(S), count, hedge.data());
            for (uint32_t i = 0; i < filled; ++i) {
                const double got = static_cast<double>(book.last_delta[i]);
                const double m = std::fabs(std::log(gS[i] / gK[i]));
                overall.add(got, want[i], gS[i], gK[i], gSig[i], gT[i]);
                if (m <= 0.05)      atm.add(got, want[i], gS[i], gK[i], gSig[i], gT[i]);
                else if (m <= 0.20) near_.add(got, want[i], gS[i], gK[i], gSig[i], gT[i]);
                else                far.add(got, want[i], gS[i], gK[i], gSig[i], gT[i]);
            }
            filled = 0;
        };

        for (double K : STRIKES)
          for (double sig : VOLS)
            for (double r : RATES)
              for (double T : EXPS)
                for (double ty : TYPES) {
                    book.strikes[filled]   = static_cast<float>(K);
                    book.expiries[filled]  = static_cast<float>(T);
                    book.iv[filled]        = static_cast<float>(sig);
                    book.rates[filled]     = static_cast<float>(r);
                    book.types[filled]     = static_cast<float>(ty);
                    book.positions[filled] = 1.0f;
                    gS[filled] = S; gK[filled] = K; gSig[filled] = sig; gT[filled] = T;
                    want[filled] = ref_delta(S, K, sig, r, T, ty);
                    ++filled;
                    if (filled == BATCH) flush();
                }
        flush();
    }

    std::printf("\n  2. DELTA ACCURACY vs double-precision std::erfc reference\n");
    std::printf("     grid: %zu spots x %zu strikes x %zu vols x %zu rates x %zu expiries x 2 types\n",
                std::size(SPOTS), std::size(STRIKES), std::size(VOLS),
                std::size(RATES), std::size(EXPS));
    auto report = [](const char* name, const ErrStats& e) {
        std::printf("     %-28s n=%-7zu  mean|err|=%.3e  max|err|=%.3e\n",
                    name, e.n, e.mean(), e.max_abs);
        if (e.n > 0) {
            std::printf("     %-28s worst at S=%.0f K=%.0f sigma=%.2f T=%.4f\n",
                        "", e.worst_S, e.worst_K, e.worst_sig, e.worst_T);
        }
        if (e.nan_kernel > 0) {
            std::printf("     %-28s *** KERNEL RETURNED NON-FINITE DELTA on %zu of %zu inputs ***\n",
                        "", e.nan_kernel, e.nan_kernel + e.n);
            std::printf("     %-28s     first at S=%.0f K=%.0f sigma=%.2f T=%.4f\n",
                        "", e.nan_S, e.nan_K, e.nan_sig, e.nan_T);
        }
        if (e.nan_ref > 0) {
            std::printf("     %-28s (reference non-finite on %zu inputs — excluded)\n", "", e.nan_ref);
        }
    };
    report("ALL", overall);
    report("at-the-money |ln(S/K)|<=0.05", atm);
    report("near        <=0.20", near_);
    report("far          >0.20", far);

    emit_csv_scalar("blackscholes", "delta_nonfinite_count",
                    static_cast<double>(overall.nan_kernel), "count",
                    "inputs where the kernel returned NaN/Inf instead of a delta");
    emit_csv_scalar("blackscholes", "delta_max_abs_err", overall.max_abs, "abs", "full grid, finite results only");
    emit_csv_scalar("blackscholes", "delta_mean_abs_err", overall.mean(), "abs", "full grid");
    emit_csv_scalar("blackscholes", "delta_max_abs_err_atm", atm.max_abs, "abs", "|ln(S/K)|<=0.05");
    emit_csv_scalar("blackscholes", "delta_max_abs_err_far", far.max_abs, "abs", "|ln(S/K)|>0.20");

    // ── 2b. If the kernel produced non-finite output, locate the edge ──
    if (overall.nan_kernel > 0) {
        std::printf("\n  2b. NON-FINITE BOUNDARY — sweeping moneyness to find where it breaks\n");
        std::printf("     (S fixed at 500, sigma 0.20, T 0.25, call)\n");
        std::printf("     %-10s %-12s %-16s %-16s\n", "K", "ln(S/K)", "kernel delta", "reference");
        for (double K : {500.0, 600.0, 700.0, 800.0, 900.0, 1000.0, 1200.0, 1500.0, 2000.0}) {
            for (uint32_t i = 0; i < 8; ++i) {
                book.strikes[i] = static_cast<float>(K);
                book.expiries[i] = 0.25f; book.iv[i] = 0.20f; book.rates[i] = 0.02f;
                book.types[i] = 1.0f;     book.positions[i] = 1.0f;
            }
            book.last_delta.fill(0.0f);
            optirisk::compute::compute_options_m2m(&book, 500.0f, 8, hedge.data());
            const double got = static_cast<double>(book.last_delta[0]);
            const double wnt = ref_delta(500.0, K, 0.20, 0.02, 0.25, 1.0);
            std::printf("     %-10.0f %-12.4f %-16.9g %-16.9g%s\n",
                        K, std::log(500.0 / K), got, wnt,
                        std::isfinite(got) ? "" : "   <-- NON-FINITE");
        }
    }

    // ── 2c. Tail coverage ──────────────────────────────────────────
    //
    // The AVX2 loop covers count & ~7. With count=500 that is 496, so the
    // last 4 options are the tail. This writes a sentinel into every output
    // slot first, then checks that the kernel overwrote all 500 — which is
    // exactly what the tail bug failed to do.
    {
        std::printf("\n  2c. TAIL COVERAGE — count=500 (not a multiple of 8)\n");
        constexpr uint32_t N = 500;
        constexpr float SENTINEL = -12345.0f;
        for (uint32_t i = 0; i < N; ++i) {
            book.strikes[i]   = 480.0f + static_cast<float>(i % 40);
            book.expiries[i]  = 0.25f;
            book.iv[i]        = 0.20f;
            book.rates[i]     = 0.02f;
            book.types[i]     = 1.0f;
            book.positions[i] = 1000.0f;
            book.last_delta[i] = SENTINEL;
            hedge[i] = SENTINEL;
        }
        optirisk::compute::compute_options_m2m(&book, 500.0f, N, hedge.data());

        uint32_t unpriced = 0;
        uint32_t first_unpriced = N;
        for (uint32_t i = 0; i < N; ++i) {
            if (book.last_delta[i] == SENTINEL) {
                ++unpriced;
                if (first_unpriced == N) first_unpriced = i;
            }
        }
        std::printf("     vector loop covers : %u of %u\n", N & ~7u, N);
        std::printf("     options left unpriced: %u%s\n", unpriced,
                    (unpriced == 0) ? "  <-- tail is covered" : "  <-- TAIL BUG PRESENT");
        if (unpriced > 0) {
            std::printf("     first unpriced index : %u\n", first_unpriced);
        }
        emit_csv_scalar("blackscholes", "unpriced_tail_options",
                        static_cast<double>(unpriced), "count",
                        "count=500; options the kernel never wrote");
    }

    // ── 3. Fast-log accuracy in isolation ──────────────────────────
    std::printf("\n  3. FAST-LOG SERIES vs std::log — error by S/K ratio\n");
    std::printf("     %-10s %-16s %-16s %-12s\n", "S/K", "series", "std::log", "abs err");
    double logmax = 0.0;
    for (double ratio : {0.70, 0.80, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20, 1.30, 1.50}) {
        const double a = fast_log_series(ratio);
        const double b = std::log(ratio);
        const double e = std::fabs(a - b);
        if (e > logmax) logmax = e;
        std::printf("     %-10.2f %-16.9f %-16.9f %-12.3e\n", ratio, a, b, e);
    }
    emit_csv_scalar("blackscholes", "fastlog_max_abs_err", logmax, "abs", "S/K in [0.70, 1.50]");

    // ── 4. Reciprocal accuracy ─────────────────────────────────────
    std::printf("\n  4. RECIPROCAL: _mm256_rcp_ps + 1 Newton-Raphson step vs true division\n");
#if defined(HAS_AVX2)
    {
        double rmax = 0.0, rsum = 0.0;
        std::size_t rn = 0;
        for (double p = 1.0; p <= 2000.0; p += 0.25) {
            alignas(32) float in[8];
            alignas(32) float out[8];
            for (int i = 0; i < 8; ++i) in[i] = static_cast<float>(p + i * 0.03125);
            __m256 v = _mm256_load_ps(in);
            __m256 inv = _mm256_rcp_ps(v);
            inv = _mm256_mul_ps(inv, _mm256_fnmadd_ps(v, inv, _mm256_set1_ps(2.0f)));
            _mm256_store_ps(out, inv);
            for (int i = 0; i < 8; ++i) {
                const double truth = 1.0 / static_cast<double>(in[i]);
                const double rel = std::fabs((static_cast<double>(out[i]) - truth) / truth);
                if (rel > rmax) rmax = rel;
                rsum += rel;
                ++rn;
            }
        }
        std::printf("     samples=%zu  mean rel err=%.3e  max rel err=%.3e\n",
                    rn, rsum / static_cast<double>(rn), rmax);
        emit_csv_scalar("blackscholes", "rcp_nr_max_rel_err", rmax, "relative", "p in [1, 2000]");
    }
#else
    std::printf("     SKIPPED — requires x86 AVX2 (_mm256_rcp_ps has no NEON equivalent here).\n");
    emit_csv_scalar("blackscholes", "rcp_nr_max_rel_err", -1.0, "relative", "SKIPPED: not x86");
#endif

    // ── 1. Throughput ──────────────────────────────────────────────
    std::printf("\n  1. THROUGHPUT (single core)\n");
    const bool pinned = pin_to_core(bench_core(0));
    std::printf("     thread pinning: %s\n", pinned ? "ACTIVE" : "UNAVAILABLE ON THIS OS");
    report_thread_count("the throughput measurement");
    std::printf("     no threads are spawned by this binary; all three variants run\n"
                "     back to back on the calling thread over the same 496-option batch\n");

    // Fill a full BATCH of realistic positions.
    for (uint32_t i = 0; i < BATCH; ++i) {
        book.strikes[i]   = 450.0f + static_cast<float>(i % 100);
        book.expiries[i]  = 0.1f + static_cast<float>(i % 20) * 0.05f;
        book.iv[i]        = 0.15f + static_cast<float>(i % 10) * 0.05f;
        book.rates[i]     = 0.02f;
        book.types[i]     = (i % 2 == 0) ? 1.0f : -1.0f;
        book.positions[i] = 1000.0f;
    }

#ifdef OPTIRISK_BENCH_QUICK
    constexpr uint64_t WARMUP_ITERS = 200;
    constexpr uint64_t TIMED_ITERS  = 5'000;
#else
    constexpr uint64_t WARMUP_ITERS = 2'000;
    constexpr uint64_t TIMED_ITERS  = 200'000;
#endif

    for (uint64_t it = 0; it < WARMUP_ITERS; ++it) {
        optirisk::compute::compute_options_m2m(&book, 500.0f + static_cast<float>(it % 7), BATCH, hedge.data());
    }
    keep(hedge[0]);

    uint64_t t0 = read_timestamp();
    for (uint64_t it = 0; it < TIMED_ITERS; ++it) {
        optirisk::compute::compute_options_m2m(&book, 500.0f + static_cast<float>(it % 7), BATCH, hedge.data());
    }
    uint64_t kernel_ns = ticks_to_ns(read_timestamp() - t0);
    keep(hedge[0]);

    std::vector<float> sc_out(BATCH);
    std::vector<float> sK(BATCH), sT(BATCH), sSig(BATCH), sR(BATCH), sTy(BATCH);
    for (uint32_t i = 0; i < BATCH; ++i) {
        sK[i] = book.strikes[i]; sT[i] = book.expiries[i];
        sSig[i] = book.iv[i];    sR[i] = book.rates[i];  sTy[i] = book.types[i];
    }

#ifdef OPTIRISK_BENCH_QUICK
    constexpr uint64_t SCALAR_ITERS = 500;
#else
    constexpr uint64_t SCALAR_ITERS = 20'000;  // slower path — fewer iterations, same per-option accounting
#endif
    for (uint64_t it = 0; it < WARMUP_ITERS / 10; ++it) {
        scalar_delta_batch(sK.data(), sT.data(), sSig.data(), sR.data(), sTy.data(),
                           500.0f, sc_out.data(), BATCH);
    }
    keep(sc_out[0]);

    t0 = read_timestamp();
    for (uint64_t it = 0; it < SCALAR_ITERS; ++it) {
        scalar_delta_batch(sK.data(), sT.data(), sSig.data(), sR.data(), sTy.data(),
                           500.0f + static_cast<float>(it % 7), sc_out.data(), BATCH);
    }
    uint64_t scalar_ns = ticks_to_ns(read_timestamp() - t0);
    keep(sc_out[0]);

    // Middle rung: scalar, f32, identical approximations, one lane.
    std::vector<float> f32_out(BATCH);
    for (uint64_t it = 0; it < WARMUP_ITERS; ++it) {
        scalar_f32_approx_batch(sK.data(), sT.data(), sSig.data(), sR.data(), sTy.data(),
                                500.0f, f32_out.data(), BATCH);
    }
    keep(f32_out[0]);

    t0 = read_timestamp();
    for (uint64_t it = 0; it < TIMED_ITERS; ++it) {
        scalar_f32_approx_batch(sK.data(), sT.data(), sSig.data(), sR.data(), sTy.data(),
                                500.0f + static_cast<float>(it % 7), f32_out.data(), BATCH);
    }
    const uint64_t f32_ns = ticks_to_ns(read_timestamp() - t0);
    keep(f32_out[0]);

    const double kernel_ops = static_cast<double>(TIMED_ITERS * BATCH) / (static_cast<double>(kernel_ns) / 1e9);
    const double scalar_ops = static_cast<double>(SCALAR_ITERS * BATCH) / (static_cast<double>(scalar_ns) / 1e9);
    const double f32_ops    = static_cast<double>(TIMED_ITERS * BATCH) / (static_cast<double>(f32_ns) / 1e9);

    std::printf("     %-34s %14s %14s\n", "variant", "M options/s", "ns/option");
    std::printf("     %-34s %14.3f %14.2f\n", "A. scalar f64, libm std::erfc",
                scalar_ops / 1e6,
                static_cast<double>(scalar_ns) / static_cast<double>(SCALAR_ITERS * BATCH));
    std::printf("     %-34s %14.3f %14.2f\n", "B. scalar f32, same approximations",
                f32_ops / 1e6,
                static_cast<double>(f32_ns) / static_cast<double>(TIMED_ITERS * BATCH));
    std::printf("     %-34s %14.3f %14.2f\n", "C. AVX2 f32, 8 lanes (shipped)",
                kernel_ops / 1e6,
                static_cast<double>(kernel_ns) / static_cast<double>(TIMED_ITERS * BATCH));

    // The decomposition only means anything if B and C compute the same thing.
    {
        book.last_delta.fill(0.0f);
        optirisk::compute::compute_options_m2m(&book, 500.0f, BATCH, hedge.data());
        std::vector<float> ref32(BATCH);
        scalar_f32_approx_batch(sK.data(), sT.data(), sSig.data(), sR.data(), sTy.data(),
                                500.0f, ref32.data(), BATCH);
        double worst = 0.0;
        for (uint32_t i = 0; i < BATCH; ++i) {
            worst = std::max(worst, std::fabs(static_cast<double>(book.last_delta[i]) -
                                              static_cast<double>(ref32[i])));
        }
        std::printf("\n     B vs C agreement: max |delta difference| = %.3e\n", worst);
        std::printf("     (B and C must compute the same function or the split is meaningless)\n");
        emit_csv_scalar("blackscholes", "f32_scalar_vs_avx2_max_diff", worst, "abs",
                        "scalar f32 approx vs AVX2 kernel, same inputs");
    }

    const double math_x = f32_ops / scalar_ops;
    const double vec_x  = kernel_ops / f32_ops;
    std::printf("\n     DECOMPOSITION of the %.2fx total\n", kernel_ops / scalar_ops);
    std::printf("       cheaper math  (A -> B): %.2fx   f64 libm to f32 polynomial approximations\n", math_x);
    std::printf("       vectorization (B -> C): %.2fx   1 lane to 8 lanes (ceiling 8.00x)\n", vec_x);
    std::printf("       product                %.2fx   (check: total is %.2fx)\n",
                math_x * vec_x, kernel_ops / scalar_ops);

    emit_csv_scalar("blackscholes", "f32_scalar_throughput_ops", f32_ops, "options/s",
                    "scalar f32, same approximations as the AVX2 kernel, single lane");
    emit_csv_scalar("blackscholes", "speedup_math_only", math_x, "x", "f64 libm -> f32 approx, both scalar");
    emit_csv_scalar("blackscholes", "speedup_vectorization_only", vec_x, "x", "f32 approx scalar -> AVX2 8-lane");

    {
        char bscond[256];
        std::snprintf(bscond, sizeof bscond, "single core, batch=496, kernel path=%s", bs_kernel_variant());
        emit_csv_scalar("blackscholes", "kernel_throughput_ops", kernel_ops, "options/s", bscond);
    }
    emit_csv_scalar("blackscholes", "scalar_throughput_ops", scalar_ops, "options/s", "std::erfc f64, single core, batch=496");
    emit_csv_scalar("blackscholes", "speedup_vs_scalar", kernel_ops / scalar_ops, "x", "same batch");

    std::printf("\n");
    return 0;
}
