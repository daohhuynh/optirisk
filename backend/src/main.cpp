// ============================================================================
// main.cpp — OptiRisk Entry Point
//
// Bootstraps the 3-thread Disruptor pipeline:
//   Thread 1 (Network):   Simulates market shocks → shock_ring
//   Thread 2 (Compute):   CSR graph risk cascade  → tick_ring
//   Thread 3 (Broadcast): Binary WebSocket publish
//
// Zero std::mutex. Zero std::condition_variable. Zero heap allocation.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <array>
#include <algorithm>
#include <thread>
#include <chrono>
#include <random>
#include <atomic>
#include <csignal>
#include "memory/csr_graph.hpp"
#include "concurrency/disruptor.hpp"
#include "network/wire_protocol.hpp"
#include "network/ws_listener.hpp"
#include "network/udp_publisher.hpp"
#include "utils/affinity.hpp"
#include "memory/options_book.hpp"
#include "market/order_book.hpp"
#include "compute/simd_engine.hpp"
#include "compute/cascade_engine.hpp"
#include "compute/monte_carlo.hpp"

uint32_t HERO_FIRM_ID = 0;

// Forward declarations
static void load_market_binary(optirisk::memory::CSRGraph& graph);

// Magic shock_type values that aren't real market shocks but control signals.
// Keep in sync with frontend wsService.sendReset().
constexpr uint32_t SHOCK_TYPE_RESET = 0xFF;

// ── Global Shutdown Flag ───────────────────────────────────────────
static std::atomic<bool> g_running{true};

static void signal_handler([[maybe_unused]] int signum) {
    g_running.store(false, std::memory_order_relaxed);
}

// ── Thread 1: Network (WsListener) ─────────────────────────────────
//
// Blocks on uWS event loop. Parses incoming binary shocks, and writes 
// them into shock_ring. 
//
static void network_thread(optirisk::concurrency::DisruptorEngine& engine,
                           optirisk::network::WsListener& listener) {
    optirisk::utils::pin_thread_to_core(1); // Isolate Network to Core 1
    (void)engine; // WsListener pushes to engine via the on_shock callback
    listener.run(); // Blocks until shutdown
}

// ── Thread 2: Compute (Risk Cascade & VaR Engine) ──────────────────
//
// Reads ShockPayloads from shock_ring, runs the deep Cascade
// physics loop involving the CLOB, then broadcasts ONE TickDelta per
// node whose state materially changed. This is what makes the cascade
// visible on the frontend map — without per-node deltas, the user
// only ever sees the hero firm and contagion is invisible.
//
// Quiescence model:
//   run_cascade_tick() already loops internally up to MAX_CASCADE_ROUNDS
//   and bails as soon as a round produces no new defaults. We just
//   compare pre/post snapshots and broadcast every node that moved.
//
static void compute_thread(optirisk::concurrency::DisruptorEngine& engine,
                           optirisk::network::WsListener& listener,
                           optirisk::memory::CSRGraph& graph,
                           optirisk::memory::OptionsBook& options,
                           optirisk::market::CLOBEngine& clob,
                           optirisk::network::UdpPublisher& udp) {
    optirisk::utils::pin_thread_to_core(2); // Isolate Math/Physics to Core 2

    uint64_t read_seq = 0;
    uint32_t tick_counter = 0;

    // Pre/post snapshots — stack-resident, zero heap.
    alignas(64) std::array<uint8_t, optirisk::memory::MAX_NODES> pre_defaulted{};
    alignas(64) std::array<float,   optirisk::memory::MAX_NODES> pre_risk{};
    alignas(64) std::array<double,  optirisk::memory::MAX_NODES> pre_nav{};
    alignas(64) std::array<double,  optirisk::memory::MAX_NODES> pre_assets{};

    constexpr float RISK_REPORT_EPSILON = 0.005f; // ~0.5% movement threshold

    // ── Auto-tick controller ──────────────────────────────────────────
    // After every real shock we keep ticking the cascade with a zero-
    // delta payload so second-order effects (option hedge slippage,
    // liquidation price impact, contagion through the network) have
    // time to propagate. The frontend sees a steady stream of TickDeltas
    // and the contagion visibly spreads instead of freezing on round-1
    // equilibrium. We auto-tick until the system is quiescent for
    // STABLE_TICKS_REQUIRED consecutive ticks (no new defaults, no
    // material risk movement) or a real shock arrives.
    constexpr int      STABLE_TICKS_REQUIRED = 6;
    constexpr int      AUTO_TICK_INTERVAL_MS = 120;
    constexpr uint32_t AUTO_TICK_SAFETY_MAX  = 200;

    // Shared per-tick processing: snapshot → cascade → diff-broadcast.
    // Returns number of nodes that materially moved (broadcast_count).
    // Returns a packed metric: low 16 bits = broadcast_count,
    // high 16 bits = stats.total_defaults. Lets the auto-tick loop
    // detect "no new defaults AND no real risk movement" without
    // being fooled by the always-on hero firm broadcast.
    auto process_tick = [&](const optirisk::network::ShockPayload& s,
                            uint32_t this_tick_id,
                            const char* origin) -> uint32_t {
        const uint32_t N = graph.num_nodes;

        for (uint32_t i = 0; i < N; ++i) {
            pre_defaulted[i] = graph.nodes.is_defaulted[i];
            pre_risk[i]      = graph.nodes.risk_score[i];
            pre_nav[i]       = graph.nodes.nav[i];
            pre_assets[i]    = graph.nodes.total_assets[i];
        }

        const auto stats = optirisk::compute::run_cascade_tick(clob, graph, options, s);

        uint32_t broadcast_count = 0;
        for (uint32_t i = 0; i < N; ++i) {
            const uint8_t new_def  = graph.nodes.is_defaulted[i];
            const uint8_t was_def  = pre_defaulted[i];
            const float   new_risk = graph.nodes.risk_score[i];
            const float   risk_d   = new_risk - pre_risk[i];

            const bool flipped_default = (was_def == 0) && (new_def != 0);
            const bool risk_moved      = std::fabs(risk_d) >= RISK_REPORT_EPSILON;
            const bool is_hero         = (i == HERO_FIRM_ID);

            if (!flipped_default && !risk_moved && !is_hero) continue;

            auto seq = engine.tick_ring.claim(engine.broadcast_cursor, g_running);
            if (seq == UINT64_MAX) [[unlikely]] break;
            auto& tick = engine.tick_ring.get(seq);

            tick.node_id        = i;
            tick.risk_score     = new_risk;
            tick.nav            = graph.nodes.nav[i];
            tick.exposure_total = graph.nodes.total_assets[i];
            tick.delta_nav      = graph.nodes.nav[i]          - pre_nav[i];
            tick.delta_exposure = graph.nodes.total_assets[i] - pre_assets[i];
            tick.is_defaulted   = (new_def != 0) ? 1 : 0;
            tick.hub_id         = static_cast<uint8_t>(graph.nodes.hub_id[i]);
            tick.cascade_depth  = static_cast<uint8_t>(std::min(stats.rounds, 255u));
            tick.tick_seq       = this_tick_id;
            tick.compute_ns     = stats.compute_ns;

            engine.tick_ring.publish(seq);
            ++broadcast_count;
        }

        if (stats.total_defaults > 0 || broadcast_count > 0) {
            std::printf("[%s] tick=%u rounds=%u defaults=%u liquidations=%u "
                        "slippage=%.4f bcast=%u compute=%lluns\n",
                        origin, this_tick_id, stats.rounds, stats.total_defaults,
                        stats.total_liquidations, stats.total_slippage,
                        broadcast_count,
                        static_cast<unsigned long long>(stats.compute_ns));
            std::fflush(stdout);
        }

        clob.flip_buffers();
        // High 16 bits = real defaults this tick; low 16 = broadcast count.
        return (std::min(stats.total_defaults, 0xFFFFu) << 16) | std::min(broadcast_count, 0xFFFFu);
    };

    while (g_running.load(std::memory_order_relaxed)) [[likely]] {
        if (!engine.shock_ring.available(read_seq)) [[unlikely]] {
            #if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
            #elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
            #endif
            continue;
        }

        const auto& shock = engine.shock_ring.get(read_seq);

        // ── RESET PATH ──────────────────────────────────────────────
        // Frontend pressed RESET. Reload baseline graph state and emit
        // a TickDelta for every node so the map repaints to "healthy".
        if (shock.shock_type == SHOCK_TYPE_RESET) {
            std::printf("[compute] tick=%u RESET received, reloading baseline\n",
                        tick_counter);
            std::fflush(stdout);

            load_market_binary(graph);
            const uint32_t this_tick = tick_counter++;

            for (uint32_t i = 0; i < graph.num_nodes; ++i) {
                auto tick_seq = engine.tick_ring.claim(engine.broadcast_cursor, g_running);
                if (tick_seq == UINT64_MAX) [[unlikely]] break;
                auto& tick = engine.tick_ring.get(tick_seq);

                tick.node_id        = i;
                tick.risk_score     = graph.nodes.risk_score[i];
                tick.nav            = graph.nodes.nav[i];
                tick.exposure_total = graph.nodes.total_assets[i];
                tick.delta_nav      = 0.0;
                tick.delta_exposure = 0.0;
                tick.is_defaulted   = (graph.nodes.is_defaulted[i] != 0) ? 1 : 0;
                tick.hub_id         = static_cast<uint8_t>(graph.nodes.hub_id[i]);
                tick.cascade_depth  = 0;
                tick.tick_seq       = this_tick;
                tick.compute_ns     = 0;

                engine.tick_ring.publish(tick_seq);
            }

            ++read_seq;
            engine.compute_cursor.value.store(read_seq, std::memory_order_release);
            ++engine.compute_count;
            continue;
        }

        // ── VaR REQUEST PATH ─────────────────────────────────────────
        if (shock.shock_type == 0xFE) {
            std::printf("[compute] tick=%u VaR Request received for node %u\n", tick_counter, shock.target_node_id);
            std::fflush(stdout);

            const auto var_result = optirisk::compute::run_monte_carlo_var(graph, shock);

            optirisk::network::VaRReport var_rep{};
            var_rep.target_node = shock.target_node_id;
            var_rep.paths_run   = var_result.paths_run;
            var_rep.var_95      = var_result.var_95[shock.target_node_id];

            listener.broadcast_var(var_rep);
            udp.broadcast_var(var_rep);

            std::printf("[compute] tick=%u VaR computed and broadcasted. paths=%u var_95=%.2f\n", 
                        tick_counter, var_rep.paths_run, var_rep.var_95);
            std::fflush(stdout);

            ++read_seq;
            engine.compute_cursor.value.store(read_seq, std::memory_order_release);
            ++engine.compute_count;
            continue;
        }

        std::printf("[compute] tick=%u shock recv'd, starting cascade\n", tick_counter);
        std::fflush(stdout);

        // 2-4. Snapshot, cascade, and per-node TickDelta diff broadcast.
        const uint32_t this_tick = tick_counter++;
        process_tick(shock, this_tick, "cascade");

        ++read_seq;
        engine.compute_cursor.value.store(read_seq, std::memory_order_release);
        ++engine.compute_count;

        // ── Auto-tick until quiescent ──────────────────────────────────
        // Keep evolving the system at AUTO_TICK_INTERVAL_MS so the user
        // can watch the contagion spread. A "stable" tick is one where
        // no node moved enough to broadcast. After STABLE_TICKS_REQUIRED
        // consecutive stable ticks we declare the market settled.
        // A real shock arriving on the ring breaks us out immediately.
        optirisk::network::ShockPayload noop{};
        noop.target_node_id = 0xFFFFFFFFu;
        noop.shock_type     = 0;

        int stable_streak = 0;
        for (uint32_t auto_i = 0;
             auto_i < AUTO_TICK_SAFETY_MAX && stable_streak < STABLE_TICKS_REQUIRED;
             ++auto_i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(AUTO_TICK_INTERVAL_MS));
            if (!g_running.load(std::memory_order_relaxed)) break;
            // If a real shock is queued, let the outer loop pick it up.
            if (engine.shock_ring.available(read_seq)) break;

            const uint32_t auto_tick = tick_counter++;
            const uint32_t metric        = process_tick(noop, auto_tick, "autotick");
            const uint32_t real_defaults = metric >> 16;
            const uint32_t bcast         = metric & 0xFFFFu;
            // "Stable" = no new defaults AND nothing moved beyond hero
            // (hero always broadcasts; allow a tiny noise floor).
            if (real_defaults == 0 && bcast <= 2) ++stable_streak;
            else                                  stable_streak = 0;
        }
    }
}

// ── Thread 3: Broadcast (Binary Publisher) ─────────────────────────
static void broadcast_thread(optirisk::concurrency::DisruptorEngine& engine,
                             optirisk::network::WsListener& listener,
                             optirisk::market::CLOBEngine& clob,
                             optirisk::network::UdpPublisher& udp) {
    optirisk::utils::pin_thread_to_core(3); // Isolate Exgress Network to Core 3

    uint64_t read_seq = 0;
    uint64_t last_report_seq = 0;
    auto last_report = std::chrono::steady_clock::now();

    // The BBO batch in the inactive buffer belongs to ONE compute tick, but a
    // tick fans out into many TickDeltas (one per node that moved). Track the
    // tick we last flushed BBO for so a 40-node tick sends the batch once
    // rather than 40 times.
    uint32_t last_bbo_tick = UINT32_MAX;

    while (g_running.load(std::memory_order_relaxed)) [[likely]] {
        if (!engine.tick_ring.available(read_seq)) [[unlikely]] {
            #if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
            #elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
            #endif
            continue;
        }

        const auto& tick = engine.tick_ring.get(read_seq);

        // Binary WebSocket broadcast (TCP OUCH)
        listener.broadcast_tick(tick);
        
        // UDP Multicast broadcast (UDP ITCH)
        udp.broadcast_tick(tick);

        // Blast the tick's BBO deltas via scatter-gather — once per tick.
        if (tick.tick_seq != last_bbo_tick) {
            last_bbo_tick = tick.tick_seq;
            auto bbo_deltas = clob.get_inactive_read_buffer();
            if (!bbo_deltas.empty()) {
                udp.broadcast_bbo(bbo_deltas);
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report);
        if (elapsed.count() >= 5) [[unlikely]] {
            uint64_t delta = read_seq - last_report_seq;
            double throughput = (elapsed.count() > 0)
                ? static_cast<double>(delta) / static_cast<double>(elapsed.count())
                : 0.0;

            std::printf("[broadcast] tick=%u node=%u risk=%.3f nav=%.0f "
                        "| %.0f msgs/sec | compute=%lluns "
                        "| net=%llu comp=%llu bcast=%llu\n",
                        tick.tick_seq,
                        tick.node_id,
                        static_cast<double>(tick.risk_score),
                        tick.nav,
                        throughput,
                        static_cast<unsigned long long>(tick.compute_ns),
                        static_cast<unsigned long long>(engine.network_count),
                        static_cast<unsigned long long>(engine.compute_count),
                        static_cast<unsigned long long>(engine.broadcast_count));

            last_report = now;
            last_report_seq = read_seq;
        }

        ++read_seq;
        engine.broadcast_cursor.value.store(read_seq, std::memory_order_release);
        ++engine.broadcast_count;
    }
}

// ── Load the QR Memory Binary ─────────────────────────────────────────
static void load_market_binary(optirisk::memory::CSRGraph& graph) {
    graph.clear();

    // Cold path — resolve the memory map relative to wherever the binary was
    // launched from. The documented flow is `cd backend/build && ./optirisk`,
    // which puts the repo root two levels up, but running from the repo root
    // or from backend/ should work too.
    static constexpr const char* CANDIDATE_PATHS[] = {
        "../../optirisk_memory.bin",  // backend/build  (documented working dir)
        "../optirisk_memory.bin",     // backend/
        "optirisk_memory.bin",        // repo root
    };

    FILE* fp = nullptr;
    for (const char* path : CANDIDATE_PATHS) {
        fp = std::fopen(path, "rb");
        if (fp) {
            std::printf("[init] Loading market state from %s\n", path);
            break;
        }
    }
    if (!fp) {
        std::fprintf(stderr,
                     "FATAL: Could not locate optirisk_memory.bin (searched ../../, ../, ./).\n"
                     "       Run `python scripts/infer_network.py` from the repo root first.\n");
        std::exit(1);
    }

    // We expect exactly 500 nodes based on our data pipeline
    constexpr size_t N = optirisk::memory::MAX_NODES;
    constexpr size_t E = optirisk::memory::MAX_EDGES;
    
    // Read NodeData arrays directly bypassing struct padding
    std::fread(graph.nodes.risk_score.data(), sizeof(float), N, fp);
    std::fread(graph.nodes.is_defaulted.data(), sizeof(uint8_t), N, fp);
    std::fread(graph.nodes.is_hero_firm.data(), sizeof(uint8_t), N, fp);
    std::fread(graph.nodes.equities_exposure.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.real_estate_exposure.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.crypto_exposure.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.treasuries_exposure.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.corp_bonds_exposure.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.total_assets.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.liabilities.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.nav.data(), sizeof(double), N, fp);
    std::fread(graph.nodes.credit_rating.data(), sizeof(float), N, fp);
    std::fread(graph.nodes.sector_id.data(), sizeof(uint32_t), N, fp);
    std::fread(graph.nodes.latitude.data(), sizeof(float), N, fp);
    std::fread(graph.nodes.longitude.data(), sizeof(float), N, fp);
    std::fread(graph.nodes.hub_id.data(), sizeof(uint8_t), N, fp);
    
    // Read CSREdges arrays
    std::fread(graph.edges.row_ptr.data(), sizeof(uint32_t), N + 1, fp);
    std::fread(graph.edges.col_idx.data(), sizeof(uint32_t), E, fp);
    std::fread(graph.edges.weight.data(), sizeof(double), E, fp);
    
    // Trailer
    std::fread(&graph.num_nodes, sizeof(uint32_t), 1, fp);
    std::fread(&graph.num_edges, sizeof(uint32_t), 1, fp);
    std::fread(&HERO_FIRM_ID, sizeof(uint32_t), 1, fp);
    
    std::fclose(fp);
}

// ── Main ───────────────────────────────────────────────────────────
int main() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr uint32_t NUM_NODES = 500;

    // Resolve the hardware counter's tick rate ONCE, before any telemetry is
    // recorded. The counter is not a core-frequency cycle counter on either
    // platform (see read_timestamp()), so every duration we report is derived
    // from this calibration rather than an assumed clock speed.
    optirisk::compute::calibrate_timestamp_clock();

    std::printf("═══════════════════════════════════════════\n");
    std::printf("  OptiRisk — Counterparty Risk Simulator\n");
    std::printf("  Nodes: %u | Ring: %zu slots × %zu bytes\n",
                NUM_NODES,
                optirisk::concurrency::RING_SIZE,
                sizeof(optirisk::concurrency::EventSlot<optirisk::network::ShockPayload>));
    std::printf("═══════════════════════════════════════════\n");
    std::printf("[init] Timestamp counter: %.3f ns/tick (%.2f MHz)\n",
                optirisk::compute::g_ns_per_tick,
                1000.0 / optirisk::compute::g_ns_per_tick);

    // Build the counterparty graph (one-time heap-free init)
    static optirisk::memory::CSRGraph graph;  // static → .bss, zero-initialized
    load_market_binary(graph);
    std::printf("[init] Graph loaded: %u nodes, %u edges\n", graph.num_nodes, graph.num_edges);
    std::printf("[init] Hero Firm ID dynamically mapped to: %u\n", HERO_FIRM_ID);

    // Initialize CLOB
    static optirisk::market::CLOBEngine clob;
    optirisk::market::init_clob(clob);
    std::printf("[init] CLOB Engine armed with realistic depth\n");

    // Initialize OptionsBook
    static optirisk::memory::OptionsBook options;
    optirisk::memory::init_options_book(options);
    std::printf("[init] Options Engine seeded with volatile Call/Put exposure\n");

    // Create disruptor engine (static → .bss)
    static optirisk::concurrency::DisruptorEngine engine{};
    std::printf("[init] DisruptorEngine: %zu KB total ring memory\n",
                (sizeof(engine.shock_ring) + sizeof(engine.tick_ring)) / 1024);

    // Initialize UDP Multicast Publisher
    static optirisk::network::UdpPublisher udp{"239.255.0.1", 9090};

    // WsListener parsing callback
    auto on_shock = [](const optirisk::network::ShockPayload& shock) {
        auto seq = engine.shock_ring.claim(engine.compute_cursor, g_running);
        if (seq != UINT64_MAX) {
            engine.shock_ring.get(seq) = shock;
            engine.shock_ring.publish(seq);
            engine.network_cursor.value.store(seq + 1, std::memory_order_release);
            ++engine.network_count;
        }
    };
    
    optirisk::network::WsListener listener(8080, std::move(on_shock));

    // Launch pipeline threads
    std::thread t1(network_thread,   std::ref(engine), std::ref(listener));
    std::thread t2(compute_thread,   std::ref(engine), std::ref(listener), std::ref(graph), std::ref(options), std::ref(clob), std::ref(udp));
    std::thread t3(broadcast_thread, std::ref(engine), std::ref(listener), std::ref(clob), std::ref(udp));

    std::printf("[main] Pipeline running on WS port 8080. Press Ctrl+C to stop.\n\n");

    t1.join();
    t2.join();
    t3.join();

    std::printf("\n[main] Shutdown complete.\n");
    return 0;
}
