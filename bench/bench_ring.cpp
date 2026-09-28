// ============================================================================
// bench_ring.cpp — Step 2.1 (risk engine latency + throughput) and the
//                  std::mutex baseline it is claimed to beat.
//
// Four measurements, deliberately separated because they answer different
// questions and collapsing them is how misleading numbers get published:
//
//   A. Transport-only handoff, PACED.  Producer stamps the counter, publishes
//      one event, then waits out a fixed inter-arrival gap so the ring never
//      holds more than ~1 event. This isolates the Disruptor handoff. It is
//      the only number that should ever be called "ring buffer latency".
//
//   B. Transport-only handoff, PACED, over std::mutex + std::queue +
//      std::condition_variable. Identical payload, identical pacing, identical
//      sampling. This is the apples-to-apples lock-free baseline.
//
//   C. Saturated throughput for both transports. Producer runs flat out. The
//      LATENCY here is dominated by queueing delay and is reported separately
//      so it can never be confused with (A).
//
//   D. Full pipeline: event in -> run_cascade_tick() -> TickDelta published.
//      This is "risk engine latency" in the end-to-end sense. Graph and CLOB
//      state are restored between events OUTSIDE the timed region, because
//      the cascade mutates both and an unrestored second run would measure a
//      different amount of work.
//
// Build: see bench/run_all.sh
// ============================================================================

#include <atomic>
#include <algorithm>
#include <new>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "common/bench_util.hpp"
#include "common/graph_fixture.hpp"

#include "compute/cascade_engine.hpp"
#include "concurrency/disruptor.hpp"
#include "network/wire_protocol.hpp"

using namespace optirisk::bench;
using optirisk::concurrency::RingBuffer;
using optirisk::concurrency::PaddedCursor;
using optirisk::network::ShockPayload;

// ── Tunables ───────────────────────────────────────────────────────
namespace cfg {
#ifdef OPTIRISK_BENCH_QUICK
inline constexpr uint64_t WARMUP_EVENTS      = 5'000;
inline constexpr uint64_t LATENCY_EVENTS     = 50'000;
inline constexpr int      REPEATS            = 2;
inline constexpr uint64_t PACE_NS            = 2'000;
inline constexpr uint64_t THROUGHPUT_MS      = 300;
inline constexpr uint64_t PIPELINE_EVENTS    = 60;
inline constexpr uint64_t PIPELINE_WARMUP    = 10;
#else
inline constexpr uint64_t WARMUP_EVENTS      = 200'000;
inline constexpr uint64_t LATENCY_EVENTS     = 500'000;
inline constexpr int      REPEATS            = 5;   // independent runs — p99 stability test
inline constexpr uint64_t PACE_NS            = 2'000;       // 500k events/s arrival
inline constexpr uint64_t THROUGHPUT_MS      = 3'000;
inline constexpr uint64_t PIPELINE_EVENTS    = 400;
inline constexpr uint64_t PIPELINE_WARMUP    = 100;
#endif
// Sized above the heaviest case's p99, not its p50. The large-cascade case
// measures ~4.5 ms p50 and ~10 ms p99, so anything under that lets the ring
// queue and the result becomes queue depth rather than pipeline latency.
// That failure is not subtle when it happens: it produced second-scale
// "latencies" before the per-case ring reset was added.
inline constexpr uint64_t PIPELINE_PACE_NS   = 20'000'000;
}  // namespace cfg

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_pin_ok{false};

// ============================================================================
// A / C — Disruptor transport
// ============================================================================
namespace {

struct DisruptorHarness {
    RingBuffer<ShockPayload> ring{};
    PaddedCursor consumer_cursor{};
};

// Latency samples are written by the consumer into storage reserved before
// the run starts, so no allocator is ever entered inside a timed region.
void disruptor_latency(DisruptorHarness& h, std::vector<uint64_t>& out,
                       uint64_t n_events, uint64_t pace_ns) {
    out.clear();
    out.reserve(n_events);

    std::atomic<bool> consumer_ready{false};
    std::vector<uint64_t> samples;
    samples.reserve(n_events + cfg::WARMUP_EVENTS);

    std::thread consumer([&] {
        g_pin_ok.store(pin_to_core(bench_core(1)), std::memory_order_relaxed);
        consumer_ready.store(true, std::memory_order_release);

        uint64_t read_seq = 0;
        const uint64_t total = n_events + cfg::WARMUP_EVENTS;
        while (read_seq < total) {
            if (!h.ring.available(read_seq)) {
#if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
                continue;
            }
            const uint64_t now = read_timestamp();
            const ShockPayload& p = h.ring.get(read_seq);
            samples.push_back(ticks_to_ns(now - p.timestamp_ns));
            ++read_seq;
            h.consumer_cursor.value.store(read_seq, std::memory_order_release);
        }
    });

    pin_to_core(bench_core(0));
    while (!consumer_ready.load(std::memory_order_acquire)) { }

    const uint64_t total = n_events + cfg::WARMUP_EVENTS;
    for (uint64_t i = 0; i < total; ++i) {
        const uint64_t gate = read_timestamp();

        const uint64_t seq = h.ring.claim(h.consumer_cursor, g_running);
        if (seq == UINT64_MAX) break;
        ShockPayload& p = h.ring.get(seq);
        p.target_node_id  = static_cast<uint32_t>(i % 500);
        p.shock_type      = 1;
        p.equities_delta  = -0.30;
        p.timestamp_ns    = read_timestamp();   // raw ticks, converted on the far side
        h.ring.publish(seq);

        spin_until_ns(gate, pace_ns);
    }

    consumer.join();

    // Drop the warmup prefix: those samples ran against a cold branch
    // predictor and cold cache lines and are not steady-state.
    if (samples.size() > cfg::WARMUP_EVENTS) {
        out.assign(samples.begin() + static_cast<long>(cfg::WARMUP_EVENTS), samples.end());
    }
}

uint64_t disruptor_throughput(DisruptorHarness& h, uint64_t run_ms, uint64_t& produced_out) {
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};

    std::thread consumer([&] {
        pin_to_core(bench_core(1));
        ready.store(true, std::memory_order_release);
        uint64_t read_seq = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!h.ring.available(read_seq)) {
#if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
                continue;
            }
            keep(h.ring.get(read_seq).target_node_id);
            ++read_seq;
            h.consumer_cursor.value.store(read_seq, std::memory_order_release);
        }
        consumed.store(read_seq, std::memory_order_release);
    });

    pin_to_core(bench_core(0));
    while (!ready.load(std::memory_order_acquire)) { }

    const uint64_t t0 = read_timestamp();
    uint64_t produced = 0;
    while (ticks_to_ns(read_timestamp() - t0) < run_ms * 1'000'000ull) {
        const uint64_t seq = h.ring.claim(h.consumer_cursor, g_running);
        if (seq == UINT64_MAX) break;
        ShockPayload& p = h.ring.get(seq);
        p.target_node_id = static_cast<uint32_t>(produced % 500);
        p.timestamp_ns   = 0;
        h.ring.publish(seq);
        ++produced;
    }
    const uint64_t elapsed = ticks_to_ns(read_timestamp() - t0);

    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    produced_out = produced;
    std::printf("    (produced %llu, consumed %llu)\n",
                static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(consumed.load()));
    return elapsed;
}

// ============================================================================
// B — std::mutex + std::queue + std::condition_variable baseline
// ============================================================================

struct MutexQueue {
    std::mutex m;
    std::condition_variable cv;
    std::queue<ShockPayload> q;
    bool done = false;

    // Bounded to the same 1024 slots as the ring so the comparison is fair:
    // an unbounded queue would trade memory for latency and flatter its own tail.
    static constexpr std::size_t CAPACITY = 1024;
    std::condition_variable cv_space;

    void push(const ShockPayload& p) {
        std::unique_lock<std::mutex> lk(m);
        cv_space.wait(lk, [&] { return q.size() < CAPACITY || done; });
        q.push(p);
        lk.unlock();
        cv.notify_one();
    }

    bool pop(ShockPayload& out) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return !q.empty() || done; });
        if (q.empty()) return false;
        out = q.front();
        q.pop();
        lk.unlock();
        cv_space.notify_one();
        return true;
    }

    void finish() {
        {
            std::lock_guard<std::mutex> lk(m);
            done = true;
        }
        cv.notify_all();
        cv_space.notify_all();
    }
};

void mutex_latency(std::vector<uint64_t>& out, uint64_t n_events, uint64_t pace_ns) {
    out.clear();
    out.reserve(n_events);

    MutexQueue mq;
    std::vector<uint64_t> samples;
    samples.reserve(n_events + cfg::WARMUP_EVENTS);
    std::atomic<bool> ready{false};

    std::thread consumer([&] {
        pin_to_core(bench_core(1));
        ready.store(true, std::memory_order_release);
        ShockPayload p{};
        const uint64_t total = n_events + cfg::WARMUP_EVENTS;
        uint64_t seen = 0;
        while (seen < total) {
            if (!mq.pop(p)) break;
            const uint64_t now = read_timestamp();
            samples.push_back(ticks_to_ns(now - p.timestamp_ns));
            ++seen;
        }
    });

    pin_to_core(bench_core(0));
    while (!ready.load(std::memory_order_acquire)) { }

    const uint64_t total = n_events + cfg::WARMUP_EVENTS;
    ShockPayload p{};
    p.shock_type = 1;
    p.equities_delta = -0.30;
    for (uint64_t i = 0; i < total; ++i) {
        const uint64_t gate = read_timestamp();
        p.target_node_id = static_cast<uint32_t>(i % 500);
        p.timestamp_ns   = read_timestamp();
        mq.push(p);
        spin_until_ns(gate, pace_ns);
    }
    mq.finish();
    consumer.join();

    if (samples.size() > cfg::WARMUP_EVENTS) {
        out.assign(samples.begin() + static_cast<long>(cfg::WARMUP_EVENTS), samples.end());
    }
}

uint64_t mutex_throughput(uint64_t run_ms, uint64_t& produced_out) {
    MutexQueue mq;
    std::atomic<bool> ready{false};
    std::atomic<uint64_t> consumed{0};

    std::thread consumer([&] {
        pin_to_core(bench_core(1));
        ready.store(true, std::memory_order_release);
        ShockPayload p{};
        uint64_t seen = 0;
        while (mq.pop(p)) { keep(p.target_node_id); ++seen; }
        consumed.store(seen, std::memory_order_release);
    });

    pin_to_core(bench_core(0));
    while (!ready.load(std::memory_order_acquire)) { }

    ShockPayload p{};
    const uint64_t t0 = read_timestamp();
    uint64_t produced = 0;
    while (ticks_to_ns(read_timestamp() - t0) < run_ms * 1'000'000ull) {
        p.target_node_id = static_cast<uint32_t>(produced % 500);
        mq.push(p);
        ++produced;
    }
    const uint64_t elapsed = ticks_to_ns(read_timestamp() - t0);

    mq.finish();
    consumer.join();
    produced_out = produced;
    std::printf("    (produced %llu, consumed %llu)\n",
                static_cast<unsigned long long>(produced),
                static_cast<unsigned long long>(consumed.load()));
    return elapsed;
}

// ============================================================================
// D — Full pipeline: event -> cascade -> TickDelta published
// ============================================================================

struct PipelineState {
    optirisk::memory::CSRGraph graph{};
    optirisk::memory::OptionsBook options{};
    optirisk::market::CLOBEngine clob{};
    GraphSnapshot baseline{};
};

static PipelineState g_pipeline;

struct PipelineCase {
    const char* name;
    double eq, re, cr, tr, cb;
};

// Three regimes on the real graph, chosen by how much cascade they produce.
// Rounds and default counts are reported alongside the latency, because the
// cost of run_cascade_tick is a function of both.
constexpr PipelineCase PIPELINE_CASES[] = {
    {"no defaults",    -0.30,  0.00,  0.00,  0.00,  0.00},
    {"small cascade",  -0.80,  0.00,  0.00,  0.00,  0.00},
    {"large cascade",  -0.50, -0.50, -0.50, -0.50, -0.50},
};

static uint32_t g_last_rounds = 0;
static uint32_t g_last_defaults = 0;

void pipeline_latency(std::vector<uint64_t>& out, uint64_t n_events,
                      const PipelineCase& pc) {
    out.clear();
    out.reserve(n_events);

    // These are static only to keep 128 KB of ring off the stack. They MUST be
    // reset per call: the write cursor persists across calls while the consumer
    // restarts at read_seq 0, so without this the second case reads the first
    // case's payloads out of stale slots and the consumer runs a full run
    // behind the producer, turning the measurement into queue depth. That bug
    // produced 2-round/0-default results and second-scale "latencies" before it
    // was caught.
    static DisruptorHarness h;
    static RingBuffer<optirisk::network::TickDelta> tick_ring;
    static PaddedCursor tick_consumer;
    h.~DisruptorHarness();
    new (&h) DisruptorHarness();
    tick_ring.~RingBuffer();
    new (&tick_ring) RingBuffer<optirisk::network::TickDelta>();
    tick_consumer.value.store(0, std::memory_order_relaxed);

    // Guard: every consumed payload must be the shock this case asked for.
    // A mismatch means the transport handed back something stale, which is
    // exactly the failure above, and it should abort rather than be averaged.
    std::atomic<uint64_t> wrong_payloads{0};

    std::vector<uint64_t> samples;
    samples.reserve(n_events + cfg::PIPELINE_WARMUP);
    std::atomic<bool> ready{false};

    std::thread compute([&] {
        pin_to_core(bench_core(1));
        ready.store(true, std::memory_order_release);

        uint64_t read_seq = 0;
        const uint64_t total = n_events + cfg::PIPELINE_WARMUP;
        while (read_seq < total) {
            if (!h.ring.available(read_seq)) {
#if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
                continue;
            }
            const ShockPayload& shock = h.ring.get(read_seq);
            if (shock.equities_delta != pc.eq || shock.crypto_delta != pc.cr) {
                wrong_payloads.fetch_add(1, std::memory_order_relaxed);
            }
            const uint64_t t0 = shock.timestamp_ns;

            const auto stats = optirisk::compute::run_cascade_tick(
                g_pipeline.clob, g_pipeline.graph, g_pipeline.options, shock);

            // Publish the result — this is the instant the risk state is
            // visible downstream, so it is where the clock stops.
            const uint64_t tseq = tick_ring.claim(tick_consumer, g_running);
            if (tseq == UINT64_MAX) break;
            auto& t = tick_ring.get(tseq);
            t.node_id       = shock.target_node_id;
            t.risk_score    = g_pipeline.graph.nodes.risk_score[shock.target_node_id];
            t.nav           = g_pipeline.graph.nodes.nav[shock.target_node_id];
            t.is_defaulted  = g_pipeline.graph.nodes.is_defaulted[shock.target_node_id] ? 1 : 0;
            t.cascade_depth = static_cast<uint8_t>(std::min(stats.rounds, 255u));
            t.compute_ns    = stats.compute_ns;
            tick_ring.publish(tseq);

            g_last_rounds   = stats.rounds;
            g_last_defaults = stats.total_defaults;

            samples.push_back(ticks_to_ns(read_timestamp() - t0));

            ++read_seq;
            h.consumer_cursor.value.store(read_seq, std::memory_order_release);
            tick_consumer.value.store(read_seq, std::memory_order_release);

            // UNTIMED: put the world back so the next event does the same work.
            g_pipeline.baseline.restore(g_pipeline.graph);
            reset_clob(g_pipeline.clob);
            optirisk::memory::init_options_book(g_pipeline.options);
        }
    });

    pin_to_core(bench_core(0));
    while (!ready.load(std::memory_order_acquire)) { }

    const uint64_t total = n_events + cfg::PIPELINE_WARMUP;
    for (uint64_t i = 0; i < total; ++i) {
        const uint64_t gate = read_timestamp();
        const uint64_t seq = h.ring.claim(h.consumer_cursor, g_running);
        if (seq == UINT64_MAX) break;
        ShockPayload& p = h.ring.get(seq);
        p.target_node_id    = 0;
        p.shock_type        = 1;
        p.equities_delta    = pc.eq;
        p.real_estate_delta = pc.re;
        p.crypto_delta      = pc.cr;
        p.treasuries_delta  = pc.tr;
        p.corp_bonds_delta  = pc.cb;
        p.timestamp_ns      = read_timestamp();
        h.ring.publish(seq);
        spin_until_ns(gate, cfg::PIPELINE_PACE_NS);
    }

    compute.join();

    const uint64_t bad = wrong_payloads.load(std::memory_order_relaxed);
    if (bad > 0) {
        std::fprintf(stderr,
                     "  FATAL: %llu of %llu events carried the wrong shock. The transport "
                     "returned stale slots, so these timings are meaningless.\n",
                     static_cast<unsigned long long>(bad),
                     static_cast<unsigned long long>(n_events + cfg::PIPELINE_WARMUP));
        std::abort();
    }

    if (samples.size() > cfg::PIPELINE_WARMUP) {
        out.assign(samples.begin() + static_cast<long>(cfg::PIPELINE_WARMUP), samples.end());
    }
}

}  // namespace

// ============================================================================
int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";

    print_header("bench_ring — Disruptor vs std::mutex, and full pipeline latency");

    const bool pinned = pin_to_core(bench_core(0));
    std::printf("  thread pinning: %s\n", pinned ? "ACTIVE" : "UNAVAILABLE ON THIS OS — tails are scheduler-polluted");
    if (!pinned) {
        std::printf("  *** percentiles below are NOT publication quality ***\n");
    }

    char cond[256];

    // ── A/B. Paced latency, repeated ───────────────────────────────
    //
    // Each transport is measured cfg::REPEATS times as independent runs. A p99
    // that reproduces across runs is signal; one that swings between them is
    // the machine. The per-repeat table below is what makes that visible —
    // pooling straight to one number would hide it.
    //
    auto repeated = [&](const char* name, bool use_mutex,
                        std::vector<uint64_t>& pooled) {
        std::printf("\n  %s — %d independent runs of %llu events each\n",
                    name, cfg::REPEATS, static_cast<unsigned long long>(cfg::LATENCY_EVENTS));
        std::printf("    %-8s %-12s %-12s %-12s\n", "run", "p50 (ns)", "p99 (ns)", "p99.9 (ns)");

        uint64_t p99_lo = UINT64_MAX, p99_hi = 0;
        for (int r = 0; r < cfg::REPEATS; ++r) {
            std::vector<uint64_t> s;
            if (use_mutex) {
                mutex_latency(s, cfg::LATENCY_EVENTS, cfg::PACE_NS);
            } else {
                static DisruptorHarness h;
                h.~DisruptorHarness();
                new (&h) DisruptorHarness();   // fresh cursors per run
                disruptor_latency(h, s, cfg::LATENCY_EVENTS, cfg::PACE_NS);
            }
            const Stats st = summarize(s);
            std::printf("    %-8d %-12llu %-12llu %-12llu\n", r + 1,
                        static_cast<unsigned long long>(st.p50_ns),
                        static_cast<unsigned long long>(st.p99_ns),
                        static_cast<unsigned long long>(st.p999_ns));
            p99_lo = std::min(p99_lo, st.p99_ns);
            p99_hi = std::max(p99_hi, st.p99_ns);
            pooled.insert(pooled.end(), s.begin(), s.end());
        }

        const double spread = (p99_lo > 0)
            ? (static_cast<double>(p99_hi - p99_lo) / static_cast<double>(p99_lo)) * 100.0
            : 0.0;
        std::printf("    p99 across runs: %llu-%llu ns (spread %.1f%%) -> %s\n",
                    static_cast<unsigned long long>(p99_lo),
                    static_cast<unsigned long long>(p99_hi), spread,
                    (spread <= 15.0) ? "REPRODUCIBLE, p99 is quotable"
                                     : "NOT REPRODUCIBLE, quote p50 only");
        char c[128];
        std::snprintf(c, sizeof c, "p99 spread across %d runs", cfg::REPEATS);
        emit_csv_scalar("ring", use_mutex ? "mutex_p99_spread_pct" : "disruptor_p99_spread_pct",
                        spread, "percent", c);
    };

    {
        std::vector<uint64_t> pooled;
        repeated("A. Disruptor handoff latency", false, pooled);
        const Stats st = summarize(pooled);
        std::snprintf(cond, sizeof cond,
                      "SPSC 1024-slot ring; paced %lluns (queue depth ~1); %d runs pooled; pinned=%d",
                      static_cast<unsigned long long>(cfg::PACE_NS), cfg::REPEATS, pinned);
        print_stats("A. Disruptor handoff latency (pooled across runs)", st, cond);
        dump_samples(outdir + "/raw_disruptor_handoff.csv", pooled);
        emit_csv_stats("ring", "disruptor_handoff_paced", st, cond);
    }

    {
        std::vector<uint64_t> pooled;
        repeated("B. Mutex+queue handoff latency", true, pooled);
        const Stats st = summarize(pooled);
        std::snprintf(cond, sizeof cond,
                      "std::mutex + std::queue + condition_variable, 1024-bounded; paced %lluns; %d runs pooled; pinned=%d",
                      static_cast<unsigned long long>(cfg::PACE_NS), cfg::REPEATS, pinned);
        print_stats("B. Mutex+queue handoff latency (pooled across runs)", st, cond);
        dump_samples(outdir + "/raw_mutex_handoff.csv", pooled);
        emit_csv_stats("ring", "mutex_handoff_paced", st, cond);
    }

    // ── C. Saturated throughput ────────────────────────────────────
    {
        static DisruptorHarness h;
        std::printf("\n  C. Saturated throughput (producer flat out — latency here is queueing, not handoff)\n");
        uint64_t dprod = 0;
        const uint64_t elapsed = disruptor_throughput(h, cfg::THROUGHPUT_MS, dprod);
        std::snprintf(cond, sizeof cond, "unpaced; SPSC ring; pinned=%d", pinned);
        print_throughput("   C1. Disruptor throughput", dprod, elapsed, cond);
        emit_csv_scalar("ring", "disruptor_throughput_eps",
                        static_cast<double>(dprod) / (static_cast<double>(elapsed) / 1e9),
                        "events/s", cond);

        uint64_t mprod = 0;
        const uint64_t melapsed = mutex_throughput(cfg::THROUGHPUT_MS, mprod);
        std::snprintf(cond, sizeof cond, "unpaced; mutex+queue; pinned=%d", pinned);
        print_throughput("   C2. Mutex+queue throughput", mprod, melapsed, cond);
        emit_csv_scalar("ring", "mutex_throughput_eps",
                        static_cast<double>(mprod) / (static_cast<double>(melapsed) / 1e9),
                        "events/s", cond);
    }

    // ── D. Full pipeline ───────────────────────────────────────────
    {
        uint32_t hero = 0;
        if (!load_real_graph(g_pipeline.graph, hero)) {
            std::fprintf(stderr,
                         "\n  D. SKIPPED — optirisk_memory.bin not found.\n"
                         "     Run `python scripts/infer_network.py` from the repo root first.\n");
        } else {
            optirisk::market::init_clob(g_pipeline.clob);
            optirisk::memory::init_options_book(g_pipeline.options);
            g_pipeline.baseline.capture(g_pipeline.graph);

            std::printf("\n  graph: %u nodes, %u edges (from optirisk_memory.bin)\n",
                        g_pipeline.graph.num_nodes, g_pipeline.graph.num_edges);

            std::printf("\n  D. Full pipeline latency (ring in -> cascade -> TickDelta published)\n");
            std::printf("     %-16s %-8s %-10s %-11s %-11s\n",
                        "case", "rounds", "defaults", "p50 (ns)", "p99 (ns)");

            for (const auto& pc : PIPELINE_CASES) {
                std::vector<uint64_t> s;
                pipeline_latency(s, cfg::PIPELINE_EVENTS, pc);
                const Stats st = summarize(s);

                std::printf("     %-16s %-8u %-10u %-11llu %-11llu\n",
                            pc.name, g_last_rounds, g_last_defaults,
                            static_cast<unsigned long long>(st.p50_ns),
                            static_cast<unsigned long long>(st.p99_ns));

                std::snprintf(cond, sizeof cond,
                              "%s: %u nodes/%u edges, %u rounds, %u defaults; state restored between events (untimed); paced %lluus; pinned=%d",
                              pc.name, g_pipeline.graph.num_nodes, g_pipeline.graph.num_edges,
                              g_last_rounds, g_last_defaults,
                              static_cast<unsigned long long>(cfg::PIPELINE_PACE_NS / 1000), pinned);

                char metric[64];
                std::snprintf(metric, sizeof metric, "pipeline_%s",
                              (pc.name[0] == 'n') ? "no_defaults"
                            : (pc.name[0] == 's') ? "small_cascade" : "large_cascade");
                print_stats(pc.name, st, cond);
                dump_samples(outdir + "/raw_pipeline_" + metric + ".csv", s);
                emit_csv_stats("ring", metric, st, cond);
            }
        }
    }

    std::printf("\n");
    return 0;
}
