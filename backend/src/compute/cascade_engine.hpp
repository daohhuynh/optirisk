#pragma once
// ============================================================================
// cascade_engine.hpp — Physics Loop Engine
//
// Wraps SIMD compute, CSR Graph, and CLOB engine into a deterministic
// fixed-point cascade loop. Capped at MAX_CASCADE_ROUNDS (1024) rounds.
//
// Flow: (Shock) → CLOB slippage → Mark-to-market → Liquidation → (Contagion)
// ============================================================================

#include <cstdint>
#include <array>
#include <algorithm>

#include "memory/csr_graph.hpp"
#include "market/order_book.hpp"
#include "compute/simd_engine.hpp"
#include "compute/black_scholes_simd.hpp"

namespace optirisk::compute {

// Upper bound on cascade rounds before we declare equilibrium. Deliberately
// generous: an 80% crypto crash with slippage feedback can run > 100 rounds
// before quiescence. The loop also bails early as soon as a round produces
// no new defaults, so this is just a safety cap, not the steady-state cost.
inline constexpr uint32_t MAX_CASCADE_ROUNDS = 1024;

struct CascadeStats {
    uint32_t rounds;
    uint32_t total_defaults;
    uint32_t total_liquidations;
    double   total_slippage;
    uint64_t compute_ns;   // Wall-clock nanoseconds for the whole cascade
};

// ── Fixed-Point Cascade Simulator ───────────────────────────────────────
// Given an initial shock string from the LLM via WebSockets, this runs 
// the cascade until equilibrium or MAX_ROUNDS is hit.
__attribute__((always_inline))
inline CascadeStats run_cascade_tick(
    optirisk::market::CLOBEngine& clob,
    optirisk::memory::CSRGraph& graph,
    optirisk::memory::OptionsBook& options,
    const optirisk::network::ShockPayload& shock
) noexcept {
    const uint64_t start_ticks = read_timestamp();

    CascadeStats stats{0, 0, 0, 0.0, 0};

    // 1. Initial hit (Macro shock)
    // Update CLOB books based on the initial shock payload
    clob.books[0].apply_macro_shock(shock.equities_delta);
    clob.books[1].apply_macro_shock(shock.real_estate_delta);
    clob.books[2].apply_macro_shock(shock.crypto_delta);
    clob.books[3].apply_macro_shock(shock.treasuries_delta);
    clob.books[4].apply_macro_shock(shock.corp_bonds_delta);

    // Loop continues as long as the system is still moving — either a new
    // default fired this round, OR a non-defaulted node's risk score moved
    // (stress contagion is still propagating). If neither is true the system
    // has reached its post-shock equilibrium and further iteration is wasted.
    bool keep_iterating = true;
    uint32_t current_round = 0;
    uint32_t total_risk_movements = 0;

    // Buffer to hold newly defaulted nodes to liquidate next round.
    // Zero allocation: stacked array exactly sized for the worst case.
    std::array<uint32_t, optirisk::memory::MAX_NODES> liquidation_queue{};
    uint32_t liquidations_queued = 0;

    // Fetch zero-allocation BBO trace buffer for recording limit depth alterations
    optirisk::network::BboUpdate* bbo_buf = nullptr;
    uint32_t* bbo_count = nullptr;
    clob.get_write_buffer(&bbo_buf, &bbo_count);

    // 2. Cascade Loop
    while (keep_iterating && current_round < MAX_CASCADE_ROUNDS) {
        keep_iterating = false;

        // Step A: Mark-to-Market (SIMD)
        // Note: apply_shock_simd() here acts as a general delta-updater if we convert it.
        // Wait, for step-wise cascade, we need prices, not deltas. 
        // SIMD engine uses deltas. Let's calculate the fractional price drop for SIMD.
        
        optirisk::network::ShockPayload iteration_shock{};
        // The delta is vs the original baseline before the cascade started.
        // Wait, no. The exposures are in USD. The SIMD engine currently does:
        //    exposure[i] *= (1.0 + delta).
        // Since we are iterating, we must be careful not to keep multiplying the total.
        // What we really need is to pass the current CLOB prices to a price-based 
        // NAV re-calculator, or use deltas *since the last round*.
        // Using deltas since the LAST round is much easier and fits existing SIMD.
        
        // Let's compute delta since last round (we will track last_round_prices)
        double cur_eq = clob.books[0].last_price;
        double cur_re = clob.books[1].last_price;
        double cur_cr = clob.books[2].last_price;
        double cur_tr = clob.books[3].last_price;
        double cur_cb = clob.books[4].last_price;
        
        // If it's round 0, the deltas are the raw shock from the user payload.
        // The user effectively gave us the new CLOB baseline.
        if (current_round == 0) {
            iteration_shock = shock;
            // The SIMD engine will do exposure * (1+delta).
        } else {
            // For subsequent rounds, the CLOB slippage gives us a NEW delta.
            // But wait, the standard SIMD loop already applied the initial shock.
            // We need a specific SIMD re-evaluation based on current asset multipliers.
            // Let's just use the absolute multipliers vs baseline.
        }

        // STEP A.1: Option Greeks & Delta Hedging (Gamma Squeeze Mechanic)
        // Options exclusively track the Equities market in this setup.
        alignas(64) std::array<float, optirisk::memory::MAX_NODES> hedge_volumes{};
        float current_equities_price = static_cast<float>(clob.books[0].last_price);

        // Execute extreme mathematical FMA SIMD block
        compute_options_m2m(&options, current_equities_price, graph.num_nodes, hedge_volumes.data());

        // Dump resulting hedges immediately into Limit Order Book
        for (uint32_t i = 0; i < graph.num_nodes; ++i) {
            float hv = hedge_volumes[i];
            if (hv != 0.0f) {
                // To remain Delta-Neutral, node triggers market order.
                // Slippage is accounted natively by the CLOB shifting its `last_price`.
                if (hv > 0.0f) {
                    clob.books[0].market_buy(hv, bbo_buf, bbo_count, 0); 
                } else {
                    clob.books[0].market_sell(-hv, bbo_buf, bbo_count, 0);
                }
            }
        }
        
        // Re-read potentially shifted Equities price to feed the Linear M2M below
        if (current_round > 0) {
            iteration_shock.equities_delta = (clob.books[0].last_price - cur_eq) / cur_eq;
        }

        // STEP A.2: Linear Mark-to-Market (SIMD)
        // apply_shock_simd updates risk and defaults, returning new defaults count.
        auto tick_result = apply_shock_simd(graph, (current_round == 0) ? shock : iteration_shock);

        stats.total_defaults  += tick_result.cascade.defaults_triggered;
        total_risk_movements  += tick_result.cascade.risk_movements;
        if (tick_result.cascade.risk_movements > 0 ||
            tick_result.cascade.defaults_triggered > 0) {
            keep_iterating = true;
        }

        // Collect newly defaulted nodes queue
        for (uint32_t i = 0; i < graph.num_nodes; ++i) {
            // we need a strict state machine: Healthy(0) -> Defaulted(1) -> Liquidated(2)
            if (graph.nodes.is_defaulted[i] == 1) {
                liquidation_queue[liquidations_queued++] = i;
                graph.nodes.is_defaulted[i] = 2; // Mark as queued for liquidation
            }
        }

        if (liquidations_queued > 0) {
            keep_iterating = true;

            // Step B: Liquidate
            for (uint32_t q = 0; q < liquidations_queued; ++q) {
                uint32_t target_node = liquidation_queue[q];
                
                // For each asset class, convert USD exposure to units, sell on CLOB
                // Use original baseline to convert to units:
                
                for (uint8_t a = 0; a < 5; ++a) {
                    double exposure_usd = 0.0;
                    if (a == 0) exposure_usd = graph.nodes.equities_exposure[target_node];
                    else if (a == 1) exposure_usd = graph.nodes.real_estate_exposure[target_node];
                    else if (a == 2) exposure_usd = graph.nodes.crypto_exposure[target_node];
                    else if (a == 3) exposure_usd = graph.nodes.treasuries_exposure[target_node];
                    else if (a == 4) exposure_usd = graph.nodes.corp_bonds_exposure[target_node];

                    if (exposure_usd > 1.0) { // arbitrary tiny cutoff
                        double baseline = clob.books[a].original_baseline;
                        double units = exposure_usd / baseline;
                        
                        auto fill = clob.books[a].market_sell(units, bbo_buf, bbo_count, a);
                        stats.total_slippage += fill.slippage_pct;
                        
                        // Exposure is now seized.
                        if (a == 0) graph.nodes.equities_exposure[target_node] = 0;
                        else if (a == 1) graph.nodes.real_estate_exposure[target_node] = 0;
                        else if (a == 2) graph.nodes.crypto_exposure[target_node] = 0;
                        else if (a == 3) graph.nodes.treasuries_exposure[target_node] = 0;
                        else if (a == 4) graph.nodes.corp_bonds_exposure[target_node] = 0;
                    }
                }
                stats.total_liquidations++;
            }
            
            // Generate the iteration shock for the next round based on price drops
            // The iteration shock represents the delta from the start of THIS ROUND.
            iteration_shock.equities_delta = (clob.books[0].last_price - cur_eq) / cur_eq;
            iteration_shock.real_estate_delta = (clob.books[1].last_price - cur_re) / cur_re;
            iteration_shock.crypto_delta = (clob.books[2].last_price - cur_cr) / cur_cr;
            iteration_shock.treasuries_delta = (clob.books[3].last_price - cur_tr) / cur_tr;
            iteration_shock.corp_bonds_delta = (clob.books[4].last_price - cur_cb) / cur_cb;

            liquidations_queued = 0;
        }

        current_round++;
    }

    stats.rounds = current_round;
    (void)total_risk_movements; // tracked for future telemetry
    const uint64_t end_ticks = read_timestamp();
    stats.compute_ns = (end_ticks > start_ticks) ? ticks_to_ns(end_ticks - start_ticks) : 0;

    return stats;
}

} // namespace optirisk::compute
