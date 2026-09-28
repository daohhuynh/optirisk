// ============================================================================
// bench_ablation.cpp — which channel actually produces the defaults?
//
// The cascade has two ways for one firm's trouble to reach another:
//
//   GAMMA      option delta hedging pushes market orders into the equities
//              book, which moves the price, which marks every holder to a
//              worse NAV. Propagates through the PRICE.
//   COUNTERPARTY  a defaulting or stressed firm pushes risk along its CSR
//              debt edges to its creditors. Propagates through the NETWORK.
//
// This binary is built four times, once per corner of the 2x2:
//
//   full          both channels live               (no flags)
//   no-gamma      OPTIRISK_NO_GAMMA_FEEDBACK
//   no-counterparty  OPTIRISK_NO_COUNTERPARTY_CONTAGION
//   neither       both flags
//
// The fourth corner is not decoration. Without it "share of defaults" has no
// denominator: some firms fail on the direct shock alone and would have failed
// with both channels switched off, and attributing those to either channel
// would be wrong. With it the counts decompose properly, and whatever does not
// decompose is the interaction term, which is reported rather than hidden.
//
// CAP SWEEP. MAX_CASCADE_ROUNDS is a safety cap, and a previous measurement
// found the last default under -80% equities landing at round 952 of 1024,
// which is close enough to the ceiling to suspect truncation. Every scenario
// here is therefore run at increasing caps until the default count stops
// changing, and the cap at which it settled is reported. If a scenario is
// still moving at the largest cap, it is flagged as UNCONVERGED rather than
// being reported as though it had settled.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/bench_util.hpp"
#include "common/graph_fixture.hpp"

#include "compute/cascade_engine.hpp"

using namespace optirisk::bench;
using optirisk::network::ShockPayload;

namespace {

const char* variant_name() {
#if defined(OPTIRISK_NO_GAMMA_FEEDBACK) && defined(OPTIRISK_NO_COUNTERPARTY_CONTAGION)
    return "neither";
#elif defined(OPTIRISK_NO_GAMMA_FEEDBACK)
    return "no-gamma";
#elif defined(OPTIRISK_NO_COUNTERPARTY_CONTAGION)
    return "no-counterparty";
#else
    return "full";
#endif
}

struct Scenario {
    const char* name;
    double eq, re, cr, tr, cb;
};

constexpr Scenario SCENARIOS[] = {
    {"-10% equities",        -0.10,  0.00,  0.00,  0.00,  0.00},
    {"-30% equities",        -0.30,  0.00,  0.00,  0.00,  0.00},
    {"-50% equities",        -0.50,  0.00,  0.00,  0.00,  0.00},
    {"-80% equities",        -0.80,  0.00,  0.00,  0.00,  0.00},
    {"-80% crypto",           0.00,  0.00, -0.80,  0.00,  0.00},
    {"Lehman",               -0.40, -0.25,  0.00,  0.00, -0.15},
    {"Covid",                -0.35, -0.10, -0.50,  0.00,  0.00},
    {"-50% all classes",     -0.50, -0.50, -0.50, -0.50, -0.50},
    {"-90% all classes",     -0.90, -0.90, -0.90, -0.90, -0.90},
};

// Each cap is run to completion; the sweep stops as soon as two consecutive
// caps agree on the default count.
constexpr uint32_t CAPS[] = {1024, 2048, 4096, 8192, 16384, 32768};

struct Fixture {
    optirisk::memory::CSRGraph   graph{};
    optirisk::memory::OptionsBook options{};
    optirisk::market::CLOBEngine  clob{};
};

static Fixture g_fx;
static GraphSnapshot g_baseline;

void reset() {
    g_baseline.restore(g_fx.graph);
    optirisk::market::init_clob(g_fx.clob);
    optirisk::memory::init_options_book(g_fx.options);
}

ShockPayload make_shock(const Scenario& sc) {
    ShockPayload p{};
    p.target_node_id    = 0;
    p.shock_type        = 1;
    p.equities_delta    = sc.eq;
    p.real_estate_delta = sc.re;
    p.crypto_delta      = sc.cr;
    p.treasuries_delta  = sc.tr;
    p.corp_bonds_delta  = sc.cb;
    return p;
}

struct Settled {
    uint32_t defaults = 0;
    uint32_t last_default_round = UINT32_MAX;
    uint32_t rounds = 0;
    uint32_t settled_cap = 0;
    bool     converged = false;
    bool     cap_1024_truncated = false;
    uint32_t defaults_at_1024 = 0;
};

Settled sweep(const Scenario& sc) {
    Settled out{};
    const ShockPayload shock = make_shock(sc);

    uint32_t prev_defaults = UINT32_MAX;
    for (uint32_t cap : CAPS) {
        reset();
        const auto st = optirisk::compute::run_cascade_tick(
            g_fx.clob, g_fx.graph, g_fx.options, shock, cap);

        if (cap == 1024) out.defaults_at_1024 = st.total_defaults;

        out.defaults           = st.total_defaults;
        out.last_default_round = st.last_default_round;
        out.rounds             = st.rounds;
        out.settled_cap        = cap;

        if (prev_defaults != UINT32_MAX && st.total_defaults == prev_defaults) {
            out.converged = true;
            break;
        }
        prev_defaults = st.total_defaults;
    }
    out.cap_1024_truncated = (out.defaults != out.defaults_at_1024);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";
    (void)outdir;

    print_header("bench_ablation — contagion channel decomposition");
    std::printf("  variant: %s\n", variant_name());
    std::printf("  gamma price feedback : %s\n",
#ifdef OPTIRISK_NO_GAMMA_FEEDBACK
                "DISABLED");
#else
                "enabled");
#endif
    std::printf("  counterparty contagion: %s\n",
#ifdef OPTIRISK_NO_COUNTERPARTY_CONTAGION
                "DISABLED");
#else
                "enabled");
#endif

    uint32_t hero = 0;
    if (!load_real_graph(g_fx.graph, hero)) {
        std::fprintf(stderr, "  FATAL: optirisk_memory.bin not found.\n");
        return 1;
    }
    g_baseline.capture(g_fx.graph);
    std::printf("  graph: %u nodes, %u edges\n\n", g_fx.graph.num_nodes, g_fx.graph.num_edges);

    std::printf("  %-20s %9s %13s %8s %11s %s\n",
                "scenario", "defaults", "last default", "rounds", "settled at", "1024 cap");
    std::printf("  %s\n", std::string(86, '-').c_str());

    for (const Scenario& sc : SCENARIOS) {
        const Settled r = sweep(sc);

        char last[16];
        if (r.last_default_round == UINT32_MAX) std::snprintf(last, sizeof last, "none");
        else std::snprintf(last, sizeof last, "%u", r.last_default_round);

        const char* cap_note = r.cap_1024_truncated ? "TRUNCATED" : "adequate";
        char settled[24];
        if (r.converged) std::snprintf(settled, sizeof settled, "%u", r.settled_cap);
        else             std::snprintf(settled, sizeof settled, ">%u", r.settled_cap);

        std::printf("  %-20s %9u %13s %8u %11s %s%s\n",
                    sc.name, r.defaults, last, r.rounds, settled, cap_note,
                    r.converged ? "" : "  UNCONVERGED");

        char cond[224];
        std::snprintf(cond, sizeof cond,
                      "variant=%s; defaults=%u; last_default_round=%s; rounds=%u; settled_cap=%s; defaults_at_1024=%u; %s",
                      variant_name(), r.defaults, last, r.rounds, settled,
                      r.defaults_at_1024,
                      r.cap_1024_truncated ? "1024 cap TRUNCATED" : "1024 cap adequate");

        char metric[96];
        std::snprintf(metric, sizeof metric, "%s|%s", variant_name(), sc.name);
        emit_csv_scalar("ablation", metric, static_cast<double>(r.defaults), "defaults", cond);

        std::snprintf(metric, sizeof metric, "%s|%s|last_round", variant_name(), sc.name);
        emit_csv_scalar("ablation", metric,
                        (r.last_default_round == UINT32_MAX) ? -1.0
                                                             : static_cast<double>(r.last_default_round),
                        "round", cond);
    }

    std::printf("\n");
    return 0;
}
