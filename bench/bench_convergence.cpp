// ============================================================================
// bench_convergence.cpp — does stopping at "no new defaults" change the answer?
//
// Stopping the cascade as soon as a round produces no new default looks like a
// free win: the rounds after the last default appear to be burning time without
// changing the outcome. This binary tests that, and the answer is no.
//
// It is the reason CascadeTermination::RiskQuiescence remains the default. For each shock it runs
// both policies from an identical starting state and compares the FULL final
// state, not just the summary counters:
//
//   - total_defaults, total_liquidations, total_slippage
//   - is_defaulted[] for all 500 nodes
//   - risk_score[]   for all 500 nodes
//   - nav[]          for all 500 nodes
//
// A difference in any of those is a behaviour change and is reported as one.
// Risk scores are compared with a tolerance, since the legacy policy performs
// extra float accumulation on nodes that never default; that tolerance is
// reported alongside the result so it cannot hide a real divergence.
//
// It also reports rounds-to-convergence for each shock under both policies,
// which is the number that explains the latency difference.
// ============================================================================

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <iterator>
#include <vector>

#include "common/bench_util.hpp"
#include "common/graph_fixture.hpp"

#include "compute/cascade_engine.hpp"

using namespace optirisk::bench;
using optirisk::network::ShockPayload;

namespace {

struct Scenario {
    const char* name;
    double eq, re, cr, tr, cb;
};

// Three regimes on the real graph, plus the scenarios used elsewhere in the
// suite so the equivalence check covers more than the headline cases.
constexpr Scenario SCENARIOS[] = {
    {"-10% equities",          -0.10,  0.00,  0.00,  0.00,  0.00},
    {"-30% equities",          -0.30,  0.00,  0.00,  0.00,  0.00},
    {"-50% equities",          -0.50,  0.00,  0.00,  0.00,  0.00},
    {"-80% equities",          -0.80,  0.00,  0.00,  0.00,  0.00},
    {"-80% crypto",             0.00,  0.00, -0.80,  0.00,  0.00},
    {"Lehman (-40/-25/-15)",   -0.40, -0.25,  0.00,  0.00, -0.15},
    {"Covid (-35/-10/-50)",    -0.35, -0.10, -0.50,  0.00,  0.00},
    {"-50% all classes",       -0.50, -0.50, -0.50, -0.50, -0.50},
    {"-90% all classes",       -0.90, -0.90, -0.90, -0.90, -0.90},
};

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

struct Fixture {
    optirisk::memory::CSRGraph   graph{};
    optirisk::memory::OptionsBook options{};
    optirisk::market::CLOBEngine  clob{};
};

static Fixture g_a;   // new policy
static Fixture g_b;   // legacy policy
static GraphSnapshot g_baseline;

void reset(Fixture& f) {
    g_baseline.restore(f.graph);
    optirisk::market::init_clob(f.clob);
    optirisk::memory::init_options_book(f.options);
}

struct Divergence {
    uint32_t defaulted_mismatch = 0;
    uint32_t risk_mismatch = 0;
    uint32_t nav_mismatch = 0;
    double   max_risk_delta = 0.0;
    double   max_nav_rel_delta = 0.0;
};

// Tolerance on risk_score only. The legacy policy keeps accumulating float
// additions on surviving nodes for hundreds of extra rounds, so bit-identity
// is not the right bar; a changed DEFAULT SET would be, and that is compared
// exactly.
constexpr float  RISK_TOL    = 1e-4f;
constexpr double NAV_REL_TOL = 1e-9;

Divergence compare(const optirisk::memory::CSRGraph& a,
                   const optirisk::memory::CSRGraph& b) {
    Divergence d{};
    for (uint32_t i = 0; i < a.num_nodes; ++i) {
        if (a.nodes.is_defaulted[i] != b.nodes.is_defaulted[i]) ++d.defaulted_mismatch;

        const double rd = std::fabs(static_cast<double>(a.nodes.risk_score[i]) -
                                    static_cast<double>(b.nodes.risk_score[i]));
        if (rd > d.max_risk_delta) d.max_risk_delta = rd;
        if (rd > RISK_TOL) ++d.risk_mismatch;

        const double denom = std::max(1.0, std::fabs(b.nodes.nav[i]));
        const double nd = std::fabs(a.nodes.nav[i] - b.nodes.nav[i]) / denom;
        if (nd > d.max_nav_rel_delta) d.max_nav_rel_delta = nd;
        if (nd > NAV_REL_TOL) ++d.nav_mismatch;
    }
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";
    (void)outdir;

    print_header("bench_convergence — new termination vs legacy, same final state?");
    const bool pinned = pin_to_core(bench_core(0));
    std::printf("  thread pinning: %s\n", pinned ? "ACTIVE" : "UNAVAILABLE ON THIS OS");

    uint32_t hero = 0;
    if (!load_real_graph(g_a.graph, hero)) {
        std::fprintf(stderr, "  FATAL: optirisk_memory.bin not found.\n");
        return 1;
    }
    g_baseline.capture(g_a.graph);
    load_real_graph(g_b.graph, hero);

    std::printf("  graph: %u nodes, %u edges\n", g_a.graph.num_nodes, g_a.graph.num_edges);
    std::printf("  risk tolerance %.0e, NAV relative tolerance %.0e;"
                " default set compared exactly\n\n", RISK_TOL, NAV_REL_TOL);

    std::printf("  %-24s %-22s %-22s %s\n", "shock", "NoNewDefaults", "RiskQuiescence (shipped)", "final state");
    std::printf("  %-24s %-22s %-22s %s\n", "", "rounds / def / liq", "rounds / def / liq", "");
    std::printf("  %s\n", std::string(96, '-').c_str());

    int mismatches = 0;
    for (const Scenario& sc : SCENARIOS) {
        const ShockPayload shock = make_shock(sc);

        reset(g_a);
        const auto sa = optirisk::compute::run_cascade_tick<optirisk::compute::CascadeTermination::NoNewDefaults>(
            g_a.clob, g_a.graph, g_a.options, shock);

        reset(g_b);
        const auto sb = optirisk::compute::run_cascade_tick<optirisk::compute::CascadeTermination::RiskQuiescence>(
            g_b.clob, g_b.graph, g_b.options, shock);

        const Divergence d = compare(g_a.graph, g_b.graph);
        const bool same_counts = (sa.total_defaults == sb.total_defaults) &&
                                 (sa.total_liquidations == sb.total_liquidations);
        const bool same_state = (d.defaulted_mismatch == 0) &&
                                (d.risk_mismatch == 0) &&
                                (d.nav_mismatch == 0);
        const bool ok = same_counts && same_state;
        if (!ok) ++mismatches;

        char lhs[32], rhs[32];
        std::snprintf(lhs, sizeof lhs, "%u / %u / %u", sa.rounds, sa.total_defaults, sa.total_liquidations);
        std::snprintf(rhs, sizeof rhs, "%u / %u / %u", sb.rounds, sb.total_defaults, sb.total_liquidations);

        std::printf("  %-24s %-22s %-22s %s\n", sc.name, lhs, rhs,
                    ok ? "IDENTICAL" : "*** DIVERGED ***");

        if (sb.total_defaults > 0) {
            // The gap is the whole story: it is how many consecutive quiet
            // rounds a "stop when quiet" rule would have to tolerate.
            std::printf("      last default at round %u; longest quiet gap %u rounds"
                        " -> any stop-when-quiet rule needs patience > %u\n",
                        sb.last_default_round, sb.max_default_gap, sb.max_default_gap);
        }

        if (!ok) {
            std::printf("      default-set mismatches : %u node(s)\n", d.defaulted_mismatch);
            std::printf("      risk    mismatches     : %u node(s), max delta %.3e\n",
                        d.risk_mismatch, d.max_risk_delta);
            std::printf("      nav     mismatches     : %u node(s), max rel delta %.3e\n",
                        d.nav_mismatch, d.max_nav_rel_delta);
            std::printf("      counters               : defaults %u vs %u, liquidations %u vs %u\n",
                        sa.total_defaults, sb.total_defaults,
                        sa.total_liquidations, sb.total_liquidations);
        }

        char cond[192];
        std::snprintf(cond, sizeof cond,
                      "%s: new %u rounds / legacy %u rounds; %u defaults; %s",
                      sc.name, sa.rounds, sb.rounds, sa.total_defaults,
                      ok ? "final state identical" : "DIVERGED");
        emit_csv_scalar("convergence", sc.name,
                        static_cast<double>(sa.rounds), "rounds", cond);
        emit_csv_scalar("convergence_legacy", sc.name,
                        static_cast<double>(sb.rounds), "rounds", cond);
    }

    std::printf("\n  %s\n", std::string(96, '-').c_str());
    if (mismatches == 0) {
        std::printf("  RESULT: all %zu shocks reach an identical final state under both\n"
                    "          policies, so NoNewDefaults would be a safe default here.\n",
                    std::size(SCENARIOS));
    } else {
        std::printf("  RESULT: %d of %zu shocks DIVERGED. NoNewDefaults is NOT a safe\n"
                    "          default: defaults in this model arrive in bursts separated\n"
                    "          by long quiet stretches, so the first quiet round is not\n"
                    "          the end of the cascade. RiskQuiescence stays the default.\n",
                    mismatches, std::size(SCENARIOS));
    }
    emit_csv_scalar("convergence", "diverged_scenarios",
                    static_cast<double>(mismatches), "count",
                    "shocks whose final state differs between the two policies");

    std::printf("\n");
    return mismatches == 0 ? 0 : 1;
}
