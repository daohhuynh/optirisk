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

// The kernel's fast log, extracted verbatim for isolated error measurement.
double fast_log_series(double x) {
    const double z = (x - 1.0) / (x + 1.0);
    const double z2 = z * z;
    return 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (1.0 / 5.0 + z2 * (1.0 / 7.0))));
}

struct ErrStats {
    double max_abs = 0.0;
    double mean_abs = 0.0;
    std::size_t n = 0;
    double worst_S = 0, worst_K = 0, worst_sig = 0, worst_T = 0;

    void add(double err, double S, double K, double sig, double T) {
        const double a = std::fabs(err);
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
                const double err = static_cast<double>(book.last_delta[i]) - want[i];
                const double m = std::fabs(std::log(gS[i] / gK[i]));
                overall.add(err, gS[i], gK[i], gSig[i], gT[i]);
                if (m <= 0.05)      atm.add(err, gS[i], gK[i], gSig[i], gT[i]);
                else if (m <= 0.20) near_.add(err, gS[i], gK[i], gSig[i], gT[i]);
                else                far.add(err, gS[i], gK[i], gSig[i], gT[i]);
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
    };
    report("ALL", overall);
    report("at-the-money |ln(S/K)|<=0.05", atm);
    report("near        <=0.20", near_);
    report("far          >0.20", far);

    emit_csv_scalar("blackscholes", "delta_max_abs_err", overall.max_abs, "abs", "full grid");
    emit_csv_scalar("blackscholes", "delta_mean_abs_err", overall.mean(), "abs", "full grid");
    emit_csv_scalar("blackscholes", "delta_max_abs_err_atm", atm.max_abs, "abs", "|ln(S/K)|<=0.05");
    emit_csv_scalar("blackscholes", "delta_max_abs_err_far", far.max_abs, "abs", "|ln(S/K)|>0.20");

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

    const double kernel_ops = static_cast<double>(TIMED_ITERS * BATCH) / (static_cast<double>(kernel_ns) / 1e9);
    const double scalar_ops = static_cast<double>(SCALAR_ITERS * BATCH) / (static_cast<double>(scalar_ns) / 1e9);

    std::printf("     shipped kernel [%s]\n", bs_kernel_variant());
    std::printf("     shipped kernel        : %.3f M options/s  (%.2f ns/option)\n",
                kernel_ops / 1e6,
                static_cast<double>(kernel_ns) / static_cast<double>(TIMED_ITERS * BATCH));
    std::printf("     scalar std::erfc (f64): %.3f M options/s  (%.2f ns/option)\n",
                scalar_ops / 1e6,
                static_cast<double>(scalar_ns) / static_cast<double>(SCALAR_ITERS * BATCH));
    std::printf("     speedup               : %.2fx\n", kernel_ops / scalar_ops);

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
