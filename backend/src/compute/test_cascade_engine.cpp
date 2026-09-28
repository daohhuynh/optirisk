// ============================================================================
// test_cascade_engine.cpp — Physics Loop Validation
//
// Standalone test binary. Verifies:
//   1. Mark-to-market accuracy
//   2. Multi-round convergence
//   3. CLOB slippage feedback loop
//
// Build:
//   g++ -std=c++23 -O3 -Wall -Wextra -Werror -I src \
//       src/compute/test_cascade_engine.cpp -o build/test_cascade_engine
// ============================================================================

#include <cstdio>
#include <cstdint>

#include "memory/csr_graph.hpp"
#include "memory/options_book.hpp"
#include "market/order_book.hpp"
#include "compute/simd_engine.hpp"
#include "compute/cascade_engine.hpp"

using namespace optirisk::compute;
using namespace optirisk::market;
using namespace optirisk::memory;
using namespace optirisk::network;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, msg)                                                 \
    do {                                                                  \
        if (expr) { ++g_pass; }                                           \
        else {                                                            \
            ++g_fail;                                                     \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                                 \
    } while (0)

static void build_mini_test_graph(CSRGraph& graph) {
    graph.clear();
    graph.set_node_count(10);
    
    // Node 0: Exposed heavily to crypto
    graph.nodes.total_assets[0] = 1000.0;
    graph.nodes.crypto_exposure[0] = 800.0; // 80% crypto
    graph.nodes.equities_exposure[0] = 200.0;
    graph.nodes.liabilities[0] = 900.0; 
    graph.nodes.nav[0] = 100.0; // NAV = 100. Small drop in crypto wipes it out.

    // Node 1: Heavily exposed to Node 0's debt
    graph.nodes.total_assets[1] = 500.0;
    graph.nodes.equities_exposure[1] = 500.0;
    graph.nodes.liabilities[1] = 400.0;
    graph.nodes.nav[1] = 100.0;

    // Node 1 owes Node 0, wait no we need Node 0's default to hurt Node 1.
    // That means Node 1 is a creditor to Node 0. Node 1 holds Node 0's debt.
    // In our simplified cascade, contagion directly increases risk score of neighbors
    // Let's set it up so Node 0 has an edge to Node 1
    graph.edges.row_ptr[0] = 0;
    graph.add_edge(0, 1, 300.0); // Node 0 owes Node 1 $300
    graph.edges.row_ptr[1] = 1;
    for (int i=2; i<=10; ++i) graph.edges.row_ptr[i] = 1;
}

static void test_cascade_loop() {
    std::printf("[test] cascade loop with slippage\n");
    
    CSRGraph graph{};
    build_mini_test_graph(graph);

    CLOBEngine clob{};
    init_clob(clob);

    // The cascade prices the options book every round (gamma-squeeze path),
    // so it must be seeded even when this test only exercises linear exposure.
    OptionsBook options{};
    init_options_book(options);

    // Initial shock: -30% Crypto
    ShockPayload shock{};
    shock.target_node_id = 0;
    shock.crypto_delta = -0.30;
    shock.shock_type = 1; // Market

    CascadeStats stats = run_cascade_tick(clob, graph, options, shock);

    // Node 0 started with $800 crypto. A 30% drop is -$240.
    // NAV was $100. NAV drops to -$140 -> defaults.
    CHECK(stats.total_defaults >= 1, "At least 1 default triggered");
    CHECK(stats.rounds >= 2, "Cascade took at least 2 rounds (Hit + Liquidation)");
    CHECK(stats.total_liquidations == stats.total_defaults, "All defaulted nodes liquidated");
    CHECK(stats.total_slippage > 0.0, "Liquidations caused CLOB slippage");

    std::printf("  Rounds: %u | Defaults: %u | Liq: %u | Slip: %.4f | Compute: %lluns\n",
                stats.rounds, stats.total_defaults, stats.total_liquidations,
                stats.total_slippage, static_cast<unsigned long long>(stats.compute_ns));
}

int main() {
    // Resolve the timestamp counter rate before any compute_ns is recorded.
    calibrate_timestamp_clock();

    std::printf("═══════════════════════════════════════════\n");
    std::printf("  OptiRisk — Cascade Engine Tests\n");
    std::printf("═══════════════════════════════════════════\n\n");

    test_cascade_loop();

    std::printf("\n═══════════════════════════════════════════\n");
    std::printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    std::printf("═══════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
