// ============================================================================
// bench_noise.cpp — how far into the tail can this machine be trusted?
//
// A percentile is only signal if the machine's own interference doesn't reach
// that deep. On a dedicated, pinned, isolated box, interference is rare enough
// that p99 is real work. On a shared cloud VM, steal time and host scheduling
// can contaminate the top 1% and a quoted p99 is really a measure of the
// neighbours.
//
// This binary measures that directly. It runs a FIXED unit of work — a
// dependency-chained FMA loop with a constant instruction count, no memory
// traffic, no branches that vary — many times, and reports the distribution.
// Since every iteration does identical work, any spread is the machine, not
// the code.
//
// Reading the output:
//   p99/p50 ratio near 1.0  -> interference does not reach p99; a p99 from
//                              this machine is signal and can be quoted.
//   p99/p50 ratio large     -> interference reaches p99; quote p50 only.
//
// The same logic is applied at p90, p99 and p99.9, so the report says exactly
// which percentile this machine stops supporting.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/bench_util.hpp"

using namespace optirisk::bench;

namespace {

#ifdef OPTIRISK_BENCH_QUICK
constexpr uint64_t SAMPLES = 50'000;
#else
constexpr uint64_t SAMPLES = 2'000'000;
#endif

// A fixed-cost work unit. The chain is serial, so the compiler cannot
// reorder it away and every call executes exactly the same instructions.
// Sized to land comfortably above the timer's resolution.
__attribute__((noinline))
double fixed_work(double seed) noexcept {
    double a = seed;
    for (int i = 0; i < 256; ++i) {
        a = a * 1.0000001 + 0.0000001;
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";

    print_header("bench_noise — how deep into the tail is this machine trustworthy?");
    const bool pinned = pin_to_core(bench_core(0));
    std::printf("  thread pinning: %s\n", pinned ? "ACTIVE" : "UNAVAILABLE ON THIS OS");

    std::vector<uint64_t> samples;
    samples.reserve(SAMPLES);

    double acc = 1.0;
    for (uint64_t i = 0; i < SAMPLES / 20; ++i) acc = fixed_work(acc);
    keep(acc);

    for (uint64_t i = 0; i < SAMPLES; ++i) {
        const uint64_t t0 = read_timestamp();
        acc = fixed_work(acc);
        const uint64_t t1 = read_timestamp();
        samples.push_back(ticks_to_ns(t1 - t0));
    }
    keep(acc);

    const Stats st = summarize(samples);
    char cond[256];
    std::snprintf(cond, sizeof cond,
                  "identical 256-FMA dependency chain every iteration; all spread is machine noise; pinned=%d",
                  pinned);
    print_stats("Fixed-work unit (any spread here is interference, not code)", st, cond);
    dump_samples(outdir + "/raw_noise.csv", samples);
    emit_csv_stats("noise", "fixed_work_unit", st, cond);

    // ── The verdict ────────────────────────────────────────────────
    const double p50 = static_cast<double>(st.p50_ns);
    std::printf("\n  TAIL TRUSTWORTHINESS (inflation over p50 on identical work)\n");
    std::printf("    %-10s %-14s %-12s %s\n", "percentile", "value (ns)", "vs p50", "verdict");

    struct Row { const char* name; uint64_t v; };
    const Row rows[] = {
        {"p90",   st.p90_ns},
        {"p99",   st.p99_ns},
        {"p99.9", st.p999_ns},
        {"max",   st.max_ns},
    };

    const char* deepest_ok = "none";
    for (const Row& r : rows) {
        const double ratio = (p50 > 0.0) ? static_cast<double>(r.v) / p50 : 0.0;
        const char* verdict;
        if (ratio <= 1.15)      { verdict = "CLEAN — quotable";      deepest_ok = r.name; }
        else if (ratio <= 1.50) { verdict = "MARGINAL — caveat it"; }
        else                    { verdict = "NOISE — do not quote"; }
        char ratio_s[16];
        std::snprintf(ratio_s, sizeof ratio_s, "%.2fx", ratio);
        std::printf("    %-10s %-14llu %-12s %s\n",
                    r.name, static_cast<unsigned long long>(r.v), ratio_s, verdict);
    }

    std::printf("\n    ==> Deepest percentile this machine supports: %s\n", deepest_ok);
    std::printf("        Latency percentiles beyond that point in the other benchmarks\n"
                "        are describing this machine, not the OptiRisk code.\n");
    emit_csv_scalar("noise", "p99_over_p50",
                    (p50 > 0.0) ? static_cast<double>(st.p99_ns) / p50 : 0.0,
                    "ratio", "1.00 = interference does not reach p99");

    std::printf("\n");
    return 0;
}
