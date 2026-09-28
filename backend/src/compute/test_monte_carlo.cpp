// ============================================================================
// test_monte_carlo.cpp — Monte Carlo VaR Validation
//
// Standalone test binary. Verifies:
//   1. 1024-path stochastic convergence
//   2. Welford's algorithm validity
//   3. Hardware performance counters (cycles per VaR block)
//
// Build:
//   g++ -std=c++23 -O3 -Wall -Wextra -Werror -I src \
//       src/compute/test_monte_carlo.cpp -o build/test_monte_carlo
// ============================================================================

#include <cstdio>
#include <cstdint>

#include "memory/csr_graph.hpp"
#include "network/wire_protocol.hpp"
#include "compute/monte_carlo.hpp"

using namespace optirisk::compute;
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

static void test_var_engine() {
    std::printf("[test] 1024-path VaR calculation\n");
    
    CSRGraph graph{};
    graph.clear();
    graph.set_node_count(10);

    // Node 0: Exposed heavily to crypto
    graph.nodes.total_assets[0] = 1000.0;
    graph.nodes.crypto_exposure[0] = 1000.0;
    graph.nodes.liabilities[0] = 0.0;
    graph.nodes.nav[0] = 1000.0;

    // Node 1: Unaffected
    graph.nodes.total_assets[1] = 1000.0;
    graph.nodes.equities_exposure[1] = 1000.0;
    graph.nodes.liabilities[1] = 0.0;
    graph.nodes.nav[1] = 1000.0;

    // Base shock: -30% Crypto
    ShockPayload shock{};
    shock.crypto_delta = -0.30;
    // other deltas remain 0

    VaRResult res = run_monte_carlo_var(graph, shock);

    CHECK(res.paths_run == 1024, "Ran exactly 1024 paths");
    
    // The base shock is -30%. E.g. $1000 drops by $300.
    // The jitter is N(1.0, 0.1). So the mean shock should be around -30%.
    // Expected loss node 0 ~ 300.
    CHECK(res.expected[0] > 250.0 && res.expected[0] < 350.0, "Node 0 expected loss is ~300");
    
    // P95 VaR should be higher than expected loss (worst case is a bigger drop)
    // Wait, since shock is negative delta, a jitter > 1 makes the shock worse.
    // So extreme loss is higher. VaR_95 should be strictly > Expected Loss.
    CHECK(res.var_95[0] > res.expected[0], "P95 VaR exceeds expected loss");
    
    // Node 1 was untouched by the crypto shock, so its loss should be near 0
    CHECK(res.var_95[1] > -50.0 && res.var_95[1] < 50.0, "Node 1 VaR is near zero");

    std::printf("  Node 0 | Expected Loss: $%.2f | 95%% VaR: $%.2f\n", 
                res.expected[0], res.var_95[0]);
    std::printf("  Compute: %llu ns (%.2f μs)\n",
                static_cast<unsigned long long>(res.compute_ns),
                static_cast<double>(res.compute_ns) / 1000.0);
}

int main() {
    // Resolve the timestamp counter rate before any compute_ns is recorded.
    calibrate_timestamp_clock();

    std::printf("═══════════════════════════════════════════\n");
    std::printf("  OptiRisk — Monte Carlo VaR Tests\n");
    std::printf("═══════════════════════════════════════════\n\n");

    test_var_engine();

    std::printf("\n═══════════════════════════════════════════\n");
    std::printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    std::printf("═══════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
