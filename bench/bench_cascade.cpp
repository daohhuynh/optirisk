// ============================================================================
// bench_cascade.cpp — Step 2.4: contagion propagation cost and the
//                     Python -> C++ data handoff.
//
// Three measurements:
//   1. Full run_cascade_tick() at several graph sizes. State is restored
//      between iterations OUTSIDE the timed region, because the cascade
//      mutates the graph, the CLOB and the options book; without a restore
//      the second iteration measures an already-collapsed network.
//   2. apply_shock_simd() in isolation — the two SIMD sweeps plus the
//      scalar cascade pass, without the CLOB/options loop around them.
//      This is the number the README calls "the SIMD phases".
//   3. The Python -> C++ handoff: the real fopen + 21 x fread load, timed,
//      against an in-process memcpy of the same bytes as a floor. The
//      Python-side pickle comparison lives in bench_bridge.py.
//
// CEILING: MAX_NODES is a compile-time constant of 500 (csr_graph.hpp).
// Sizes above 500 cannot be measured without changing that constant and
// rebuilding, which is a core-code change and is deliberately not done here.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/bench_util.hpp"
#include "common/graph_fixture.hpp"

#include "compute/cascade_engine.hpp"
#include "compute/simd_engine.hpp"

using namespace optirisk::bench;

namespace {

struct Fixture {
    optirisk::memory::CSRGraph graph{};
    optirisk::memory::OptionsBook options{};
    optirisk::market::CLOBEngine clob{};
    GraphSnapshot baseline{};
};

static Fixture g_fx;

#ifdef OPTIRISK_BENCH_QUICK
constexpr uint64_t WARMUP = 20;
constexpr uint64_t ITERS  = 200;
constexpr uint64_t SYNTH_ITERS = 20;
constexpr uint64_t LOAD_ITERS = 20;
#else
constexpr uint64_t WARMUP = 200;
constexpr uint64_t ITERS  = 5'000;
// The synthetic graphs run to the 1024-round cap at roughly 3 ms each, so
// they get their own budget. 400 samples still puts 4 above p99, which is
// enough for a workload whose spread is milliseconds wide.
constexpr uint64_t SYNTH_ITERS = 400;
constexpr uint64_t LOAD_ITERS = 500;
#endif

optirisk::network::ShockPayload equities_shock_30pct() {
    optirisk::network::ShockPayload s{};
    s.target_node_id  = 0;
    s.shock_type      = 1;
    s.equities_delta  = -0.30;
    return s;
}

// Full cascade at a given graph. Restore is untimed.
Stats measure_cascade(const char* label, std::vector<uint64_t>& samples_out,
                      uint64_t iters = ITERS) {
    const auto shock = equities_shock_30pct();
    std::vector<uint64_t> samples;
    samples.reserve(iters);

    for (uint64_t i = 0; i < WARMUP; ++i) {
        g_fx.baseline.restore(g_fx.graph);
        reset_clob(g_fx.clob);
        optirisk::memory::init_options_book(g_fx.options);
        const auto st = optirisk::compute::run_cascade_tick(g_fx.clob, g_fx.graph, g_fx.options, shock);
        keep(st.rounds);
    }

    uint32_t rounds_seen = 0;
    uint32_t defaults_seen = 0;
    for (uint64_t i = 0; i < iters; ++i) {
        g_fx.baseline.restore(g_fx.graph);
        reset_clob(g_fx.clob);
        optirisk::memory::init_options_book(g_fx.options);

        const uint64_t t0 = read_timestamp();
        const auto st = optirisk::compute::run_cascade_tick(g_fx.clob, g_fx.graph, g_fx.options, shock);
        const uint64_t t1 = read_timestamp();

        samples.push_back(ticks_to_ns(t1 - t0));
        rounds_seen = st.rounds;
        defaults_seen = st.total_defaults;
    }

    std::printf("     %-14s rounds=%u defaults=%u\n", label, rounds_seen, defaults_seen);
    samples_out = samples;
    return summarize(samples);
}

// apply_shock_simd() alone — no CLOB, no options, no cascade outer loop.
Stats measure_simd_phases(std::vector<uint64_t>& samples_out) {
    const auto shock = equities_shock_30pct();
    std::vector<uint64_t> samples;
    samples.reserve(ITERS);

    for (uint64_t i = 0; i < WARMUP; ++i) {
        g_fx.baseline.restore(g_fx.graph);
        const auto r = optirisk::compute::apply_shock_simd(g_fx.graph, shock);
        keep(r.nodes_processed);
    }

    for (uint64_t i = 0; i < ITERS; ++i) {
        g_fx.baseline.restore(g_fx.graph);

        const uint64_t t0 = read_timestamp();
        const auto r = optirisk::compute::apply_shock_simd(g_fx.graph, shock);
        const uint64_t t1 = read_timestamp();

        keep(r.nodes_processed);
        samples.push_back(ticks_to_ns(t1 - t0));
    }
    samples_out = samples;
    return summarize(samples);
}

}  // namespace

int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";

    print_header("bench_cascade — contagion propagation and the Python handoff");
    const bool pinned = pin_to_core(bench_core(0));
    std::printf("  thread pinning: %s\n", pinned ? "ACTIVE" : "UNAVAILABLE ON THIS OS");
    std::printf("  NOTE: MAX_NODES is a compile-time 500 (csr_graph.hpp). Sizes above\n");
    std::printf("        500 would require changing that constant and rebuilding.\n");

    char cond[256];

    // ── 1. Scaling sweep on synthetic graphs ───────────────────────
    std::printf("\n  1. FULL CASCADE by graph size (synthetic, deterministic seed)\n");
    for (uint32_t n : {100u, 200u, 300u, 400u, 500u}) {
        build_synthetic(g_fx.graph, n, 15);
        optirisk::market::init_clob(g_fx.clob);
        optirisk::memory::init_options_book(g_fx.options);
        g_fx.baseline.capture(g_fx.graph);

        char label[32];
        std::snprintf(label, sizeof label, "N=%u E=%u", g_fx.graph.num_nodes, g_fx.graph.num_edges);

        std::vector<uint64_t> s;
        const Stats st = measure_cascade(label, s, SYNTH_ITERS);
        std::snprintf(cond, sizeof cond,
                      "synthetic N=%u E=%u; -30%% equities; state restored per iter (untimed); pinned=%d",
                      g_fx.graph.num_nodes, g_fx.graph.num_edges, pinned);
        print_stats(label, st, cond);
        emit_csv_stats("cascade", label, st, cond);
    }

    // ── 2. Real 500-node graph: full cascade + SIMD phases ─────────
    uint32_t hero = 0;
    if (!load_real_graph(g_fx.graph, hero)) {
        std::fprintf(stderr, "\n  SKIPPED real-graph measurements — optirisk_memory.bin not found.\n");
    } else {
        optirisk::market::init_clob(g_fx.clob);
        optirisk::memory::init_options_book(g_fx.options);
        g_fx.baseline.capture(g_fx.graph);

        std::printf("\n  2. REAL GRAPH from optirisk_memory.bin: %u nodes, %u edges\n",
                    g_fx.graph.num_nodes, g_fx.graph.num_edges);

        std::vector<uint64_t> s;
        const Stats st = measure_cascade("real graph", s);
        std::snprintf(cond, sizeof cond,
                      "real %u nodes/%u edges; -30%% equities on node 0; state restored per iter (untimed); pinned=%d",
                      g_fx.graph.num_nodes, g_fx.graph.num_edges, pinned);
        print_stats("run_cascade_tick (full, real graph)", st, cond);
        dump_samples(outdir + "/raw_cascade_real.csv", s);
        emit_csv_stats("cascade", "run_cascade_tick_real500", st, cond);

        std::vector<uint64_t> s2;
        const Stats st2 = measure_simd_phases(s2);
        std::snprintf(cond, sizeof cond,
                      "apply_shock_simd only (2 SIMD sweeps + scalar cascade pass); real %u nodes; pinned=%d",
                      g_fx.graph.num_nodes, pinned);
        print_stats("apply_shock_simd (SIMD phases only)", st2, cond);
        dump_samples(outdir + "/raw_simd_phases.csv", s2);
        emit_csv_stats("cascade", "apply_shock_simd_real500", st2, cond);
    }

    // ── 3. Python -> C++ handoff ───────────────────────────────────
    std::printf("\n  3. PYTHON -> C++ HANDOFF\n");
    std::printf("     Mechanism as implemented: numpy .tobytes() -> flat file ->\n");
    std::printf("     fopen + 21 sequential fread into .bss. This is a COPY, not\n");
    std::printf("     shared memory, and it runs once at startup, not on the hot path.\n");
    {
        const char* path = find_market_binary();
        if (!path) {
            std::printf("     SKIPPED — optirisk_memory.bin not found.\n");
        } else {
            static optirisk::memory::CSRGraph scratch;
            std::vector<uint64_t> load_samples;

            for (uint64_t i = 0; i < 20; ++i) { uint32_t h = 0; load_real_graph(scratch, h); }

            load_samples.reserve(LOAD_ITERS);
            for (uint64_t i = 0; i < LOAD_ITERS; ++i) {
                uint32_t h = 0;
                const uint64_t t0 = read_timestamp();
                load_real_graph(scratch, h);
                load_samples.push_back(ticks_to_ns(read_timestamp() - t0));
            }
            const Stats ls = summarize(load_samples);
            std::snprintf(cond, sizeof cond,
                          "fopen + 21 fread of %s into .bss; warm page cache; pinned=%d", path, pinned);
            print_stats("C++ side: full graph load from disk", ls, cond);
            emit_csv_stats("bridge", "cpp_fread_load", ls, cond);

            // Floor: the same bytes copied in-process, no syscall, no filesystem.
            static optirisk::memory::CSRGraph dst;
            std::vector<uint64_t> memcpy_samples;
            memcpy_samples.reserve(LOAD_ITERS);
            for (uint64_t i = 0; i < LOAD_ITERS; ++i) {
                const uint64_t t0 = read_timestamp();
                std::memcpy(&dst, &scratch, sizeof(optirisk::memory::CSRGraph));
                memcpy_samples.push_back(ticks_to_ns(read_timestamp() - t0));
            }
            keep(dst.num_nodes);
            const Stats ms = summarize(memcpy_samples);
            std::snprintf(cond, sizeof cond,
                          "in-process memcpy of sizeof(CSRGraph)=%zu bytes; the floor a true zero-copy path would beat",
                          sizeof(optirisk::memory::CSRGraph));
            print_stats("Floor: in-process memcpy of the same structure", ms, cond);
            emit_csv_stats("bridge", "memcpy_floor", ms, cond);

            emit_csv_scalar("bridge", "graph_bytes",
                            static_cast<double>(sizeof(optirisk::memory::CSRGraph)), "bytes",
                            "sizeof(CSRGraph)");
        }
    }

    std::printf("\n");
    return 0;
}
