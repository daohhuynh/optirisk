#pragma once
// ============================================================================
// graph_fixture.hpp — reproducible graph + CLOB state for the benchmarks.
//
// Two sources of graph state:
//   1. load_real_graph()  — the actual 500-node / 7500-edge network from
//                           optirisk_memory.bin, byte-for-byte the same
//                           read main.cpp performs at startup.
//   2. build_synthetic()  — deterministic graphs at a chosen node count and
//                           average degree, for the O(V+E) scaling sweep.
//
// Snapshot/restore exists because run_cascade_tick() mutates the graph and
// the CLOB. Without a restore between iterations the second measurement
// would run against an already-defaulted network and measure different work.
// Restore is always performed OUTSIDE the timed region.
// ============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>
#include <vector>

#include "market/order_book.hpp"
#include "memory/csr_graph.hpp"
#include "memory/options_book.hpp"

namespace optirisk::bench {

// Same search order main.cpp uses, plus the bench/ working directory.
inline const char* find_market_binary() noexcept {
    static constexpr const char* CANDIDATES[] = {
        "optirisk_memory.bin",
        "../optirisk_memory.bin",
        "../../optirisk_memory.bin",
        "../../../optirisk_memory.bin",
    };
    for (const char* p : CANDIDATES) {
        FILE* f = std::fopen(p, "rb");
        if (f) { std::fclose(f); return p; }
    }
    return nullptr;
}

// Mirrors load_market_binary() in main.cpp exactly. Returns false if the
// file is missing so the caller can fail loudly instead of benchmarking
// an all-zero graph.
inline bool load_real_graph(optirisk::memory::CSRGraph& graph, uint32_t& hero_id) {
    const char* path = find_market_binary();
    if (!path) return false;

    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;

    graph.clear();
    constexpr size_t N = optirisk::memory::MAX_NODES;
    constexpr size_t E = optirisk::memory::MAX_EDGES;

    auto rd = [&](void* dst, size_t elem, size_t n) {
        return std::fread(dst, elem, n, fp) == n;
    };

    bool ok = true;
    ok &= rd(graph.nodes.risk_score.data(),           sizeof(float),    N);
    ok &= rd(graph.nodes.is_defaulted.data(),         sizeof(uint8_t),  N);
    ok &= rd(graph.nodes.is_hero_firm.data(),         sizeof(uint8_t),  N);
    ok &= rd(graph.nodes.equities_exposure.data(),    sizeof(double),   N);
    ok &= rd(graph.nodes.real_estate_exposure.data(), sizeof(double),   N);
    ok &= rd(graph.nodes.crypto_exposure.data(),      sizeof(double),   N);
    ok &= rd(graph.nodes.treasuries_exposure.data(),  sizeof(double),   N);
    ok &= rd(graph.nodes.corp_bonds_exposure.data(),  sizeof(double),   N);
    ok &= rd(graph.nodes.total_assets.data(),         sizeof(double),   N);
    ok &= rd(graph.nodes.liabilities.data(),          sizeof(double),   N);
    ok &= rd(graph.nodes.nav.data(),                  sizeof(double),   N);
    ok &= rd(graph.nodes.credit_rating.data(),        sizeof(float),    N);
    ok &= rd(graph.nodes.sector_id.data(),            sizeof(uint32_t), N);
    ok &= rd(graph.nodes.latitude.data(),             sizeof(float),    N);
    ok &= rd(graph.nodes.longitude.data(),            sizeof(float),    N);
    ok &= rd(graph.nodes.hub_id.data(),               sizeof(uint8_t),  N);
    ok &= rd(graph.edges.row_ptr.data(),              sizeof(uint32_t), N + 1);
    ok &= rd(graph.edges.col_idx.data(),              sizeof(uint32_t), E);
    ok &= rd(graph.edges.weight.data(),               sizeof(double),   E);
    ok &= rd(&graph.num_nodes,                        sizeof(uint32_t), 1);
    ok &= rd(&graph.num_edges,                        sizeof(uint32_t), 1);
    ok &= rd(&hero_id,                                sizeof(uint32_t), 1);

    std::fclose(fp);
    return ok;
}

// ── Synthetic graph for the scaling sweep ──────────────────────────
//
// Preferential attachment, matching the shape generate_data.py produces:
// a small number of high-degree hubs and a long tail of leaf nodes.
// Fully deterministic for a given (n_nodes, avg_degree, seed) so the
// scaling numbers are reproducible across machines.
//
inline void build_synthetic(optirisk::memory::CSRGraph& graph,
                            uint32_t n_nodes,
                            uint32_t avg_degree,
                            uint64_t seed = 20260927ull) {
    graph.clear();
    if (n_nodes > optirisk::memory::MAX_NODES) n_nodes = optirisk::memory::MAX_NODES;
    graph.set_node_count(n_nodes);

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    // Balance sheets: Pareto-ish assets, ~85% leverage, NAV = assets - liabilities.
    for (uint32_t i = 0; i < n_nodes; ++i) {
        const double u = 0.001 + unit(rng) * 0.999;
        const double assets = 1.0e8 / std::pow(u, 1.0 / 1.6);  // Pareto alpha 1.6

        // Dirichlet-ish split across the five asset classes.
        double w[5];
        double wsum = 0.0;
        for (double& x : w) { x = unit(rng) + 0.05; wsum += x; }
        for (double& x : w) x /= wsum;

        graph.nodes.equities_exposure[i]    = assets * w[0];
        graph.nodes.real_estate_exposure[i] = assets * w[1];
        graph.nodes.crypto_exposure[i]      = assets * w[2];
        graph.nodes.treasuries_exposure[i]  = assets * w[3];
        graph.nodes.corp_bonds_exposure[i]  = assets * w[4];

        graph.nodes.total_assets[i] = assets;
        graph.nodes.liabilities[i]  = assets * (0.80 + unit(rng) * 0.10);
        graph.nodes.nav[i]          = assets - graph.nodes.liabilities[i];
        graph.nodes.risk_score[i]   = 0.0f;
        graph.nodes.is_defaulted[i] = 0;
        graph.nodes.hub_id[i]       = static_cast<optirisk::memory::HubId>(i % 5);
    }

    // Edges, CSR-ordered. Degree is drawn around avg_degree; targets are
    // chosen with probability proportional to current in-degree (hubs win).
    std::vector<uint32_t> in_degree(n_nodes, 1);
    uint32_t edge_cursor = 0;

    for (uint32_t u = 0; u < n_nodes; ++u) {
        graph.edges.row_ptr[u] = edge_cursor;

        const uint32_t lo  = (avg_degree > 2) ? avg_degree - 2 : 1;
        const uint32_t deg = lo + static_cast<uint32_t>(unit(rng) * 5.0);

        uint64_t total_in = 0;
        for (uint32_t d : in_degree) total_in += d;

        for (uint32_t k = 0; k < deg && edge_cursor < optirisk::memory::MAX_EDGES; ++k) {
            // Roulette-wheel select a target != u.
            uint64_t pick = static_cast<uint64_t>(unit(rng) * static_cast<double>(total_in));
            uint32_t target = 0;
            uint64_t acc = 0;
            for (uint32_t v = 0; v < n_nodes; ++v) {
                acc += in_degree[v];
                if (acc > pick) { target = v; break; }
            }
            if (target == u) target = (u + 1) % n_nodes;

            const double debt = graph.nodes.total_assets[u] * (0.01 + unit(rng) * 0.05);
            graph.edges.col_idx[edge_cursor] = target;
            graph.edges.weight[edge_cursor]  = debt;
            ++edge_cursor;
            ++in_degree[target];
            ++total_in;
        }
    }
    for (uint32_t u = n_nodes; u <= optirisk::memory::MAX_NODES; ++u) {
        graph.edges.row_ptr[u] = edge_cursor;
    }
    graph.edges.row_ptr[n_nodes] = edge_cursor;
    graph.num_edges = edge_cursor;
}

// ── Snapshot / restore ─────────────────────────────────────────────
//
// Only the fields run_cascade_tick() mutates. Topology (row_ptr, col_idx,
// weight) is never written by the cascade, so it is not copied.
//
struct GraphSnapshot {
    std::array<float,   optirisk::memory::MAX_NODES> risk_score{};
    std::array<uint8_t, optirisk::memory::MAX_NODES> is_defaulted{};
    std::array<double,  optirisk::memory::MAX_NODES> equities_exposure{};
    std::array<double,  optirisk::memory::MAX_NODES> real_estate_exposure{};
    std::array<double,  optirisk::memory::MAX_NODES> crypto_exposure{};
    std::array<double,  optirisk::memory::MAX_NODES> treasuries_exposure{};
    std::array<double,  optirisk::memory::MAX_NODES> corp_bonds_exposure{};
    std::array<double,  optirisk::memory::MAX_NODES> total_assets{};
    std::array<double,  optirisk::memory::MAX_NODES> liabilities{};
    std::array<double,  optirisk::memory::MAX_NODES> nav{};

    void capture(const optirisk::memory::CSRGraph& g) noexcept {
        risk_score           = g.nodes.risk_score;
        is_defaulted         = g.nodes.is_defaulted;
        equities_exposure    = g.nodes.equities_exposure;
        real_estate_exposure = g.nodes.real_estate_exposure;
        crypto_exposure      = g.nodes.crypto_exposure;
        treasuries_exposure  = g.nodes.treasuries_exposure;
        corp_bonds_exposure  = g.nodes.corp_bonds_exposure;
        total_assets         = g.nodes.total_assets;
        liabilities          = g.nodes.liabilities;
        nav                  = g.nodes.nav;
    }

    void restore(optirisk::memory::CSRGraph& g) const noexcept {
        g.nodes.risk_score           = risk_score;
        g.nodes.is_defaulted         = is_defaulted;
        g.nodes.equities_exposure    = equities_exposure;
        g.nodes.real_estate_exposure = real_estate_exposure;
        g.nodes.crypto_exposure      = crypto_exposure;
        g.nodes.treasuries_exposure  = treasuries_exposure;
        g.nodes.corp_bonds_exposure  = corp_bonds_exposure;
        g.nodes.total_assets         = total_assets;
        g.nodes.liabilities          = liabilities;
        g.nodes.nav                  = nav;
    }
};

// The CLOB is rebuilt rather than snapshotted — init_clob() is
// deterministic, so it restores exactly the same book every time.
inline void reset_clob(optirisk::market::CLOBEngine& clob) noexcept {
    optirisk::market::init_clob(clob);
}

}  // namespace optirisk::bench
