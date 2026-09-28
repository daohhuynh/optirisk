// ============================================================================
// bench_clob.cpp — Step 2.5 (CLOB fill throughput / latency, BBO broadcast)
//                  and Step 1.3 (the "O(levels consumed)" complexity claim).
//
// The complexity claim is the interesting one. "O(levels consumed)" asserts
// two things, and both are tested separately:
//
//   (a) cost grows linearly in the number of levels a fill eats, and
//   (b) cost does NOT grow with how deep the book is.
//
// (b) is what distinguishes the lazy head-increment design from an
// erase-and-shift book. A test that only varied levels consumed would pass
// for both designs and prove nothing.
//
// Order sizes are computed exactly: refresh_liquidity() lays down level i
// with depth base*(1 + i/10), so consuming exactly L levels means selling
// base * sum_{i<L}(1 + i/10) = base * (L + L(L-1)/20). Fills are issued with
// that exact size, and the returned levels_consumed is asserted against L.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/bench_util.hpp"

#include "market/order_book.hpp"
#include "network/protocol.hpp"
#include "network/udp_publisher.hpp"

using namespace optirisk::bench;
using optirisk::market::OrderBook;
using optirisk::market::CLOBEngine;

namespace {

#ifdef OPTIRISK_BENCH_QUICK
constexpr uint64_t WARMUP = 100;
constexpr uint64_t ITERS  = 5'000;
constexpr uint64_t BBO_ITERS = 2'000;
#else
constexpr uint64_t WARMUP = 2'000;
constexpr uint64_t ITERS  = 200'000;
constexpr uint64_t BBO_ITERS = 100'000;
#endif

// Units required to consume exactly `levels` price levels of a book that
// refresh_liquidity(base) just laid down.
// The tiny addend matters: refresh_liquidity() and this loop accumulate the
// same level depths in a different order, so without it the last level can be
// left with a ~1e-12 sliver and go uncounted. The next level down holds at
// least `base` units, so 1e-6 can never spill into it.
double units_for_levels(double base, uint32_t levels) {
    double total = 0.0;
    for (uint32_t i = 0; i < levels; ++i) total += base * (1.0 + static_cast<double>(i) * 0.1);
    return total + 1e-6;
}

}  // namespace

int main(int argc, char** argv) {
    optirisk::compute::calibrate_timestamp_clock();
    const std::string outdir = (argc > 1) ? argv[1] : "results";

    print_header("bench_clob — fill latency/throughput, BBO broadcast, complexity");
    const bool pinned = pin_to_core(bench_core(0));
    std::printf("  thread pinning: %s\n", pinned ? "ACTIVE" : "UNAVAILABLE ON THIS OS");

    char cond[256];
    static OrderBook book;
    constexpr double BASE_DEPTH = 10'000.0;

    // ── 1.3(a) Cost vs levels consumed, book depth fixed at 256 ────
    std::printf("\n  1.3(a) COST vs LEVELS CONSUMED (book depth fixed at 256 levels)\n");
    std::printf("     %-10s %-12s %-12s %-12s %-10s\n",
                "levels", "p50 (ns)", "p99 (ns)", "ns/level", "verified");
    for (uint32_t L : {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u}) {
        const double units = units_for_levels(BASE_DEPTH, L);
        std::vector<uint64_t> samples;
        samples.reserve(ITERS / 10);
        uint32_t observed_levels = 0;

        for (uint64_t i = 0; i < WARMUP; ++i) {
            book.last_price = 500.0;
            book.refresh_liquidity(BASE_DEPTH);
            keep(book.market_sell(units, nullptr, nullptr, 0).levels_consumed);
        }

        for (uint64_t i = 0; i < ITERS / 10; ++i) {
            book.last_price = 500.0;
            book.refresh_liquidity(BASE_DEPTH);   // UNTIMED reset

            const uint64_t t0 = read_timestamp();
            const auto fill = book.market_sell(units, nullptr, nullptr, 0);
            const uint64_t t1 = read_timestamp();

            observed_levels = fill.levels_consumed;
            samples.push_back(ticks_to_ns(t1 - t0));
        }

        const Stats st = summarize(samples);
        std::printf("     %-10u %-12llu %-12llu %-12.2f %-10s\n",
                    L,
                    static_cast<unsigned long long>(st.p50_ns),
                    static_cast<unsigned long long>(st.p99_ns),
                    static_cast<double>(st.p50_ns) / static_cast<double>(L),
                    (observed_levels == L) ? "yes" : "NO — MISMATCH");

        char metric[64];
        std::snprintf(metric, sizeof metric, "fill_levels_%u", L);
        std::snprintf(cond, sizeof cond,
                      "depth=256, levels consumed=%u (observed %u), no BBO recording; pinned=%d",
                      L, observed_levels, pinned);
        emit_csv_stats("clob", metric, st, cond);
    }

    // ── 1.3(b) Cost vs book depth, levels consumed fixed at 1 ──────
    std::printf("\n  1.3(b) COST vs BOOK DEPTH (levels consumed fixed at 1)\n");
    std::printf("     If fills are O(levels consumed), these are flat. If the book\n");
    std::printf("     shifted memory on consumption, they would grow with depth.\n");
    std::printf("     %-14s %-12s %-12s\n", "book depth", "p50 (ns)", "p99 (ns)");
    for (uint32_t depth : {8u, 16u, 32u, 64u, 128u, 256u}) {
        const double units = units_for_levels(BASE_DEPTH, 1);
        std::vector<uint64_t> samples;
        samples.reserve(ITERS / 10);

        for (uint64_t i = 0; i < WARMUP; ++i) {
            book.last_price = 500.0;
            book.refresh_liquidity(BASE_DEPTH);
            book.num_bids = depth;
            keep(book.market_sell(units, nullptr, nullptr, 0).levels_consumed);
        }

        for (uint64_t i = 0; i < ITERS / 10; ++i) {
            book.last_price = 500.0;
            book.refresh_liquidity(BASE_DEPTH);
            book.num_bids = depth;              // UNTIMED reset

            const uint64_t t0 = read_timestamp();
            const auto fill = book.market_sell(units, nullptr, nullptr, 0);
            const uint64_t t1 = read_timestamp();
            keep(fill.levels_consumed);
            samples.push_back(ticks_to_ns(t1 - t0));
        }

        const Stats st = summarize(samples);
        std::printf("     %-14u %-12llu %-12llu\n", depth,
                    static_cast<unsigned long long>(st.p50_ns),
                    static_cast<unsigned long long>(st.p99_ns));

        char metric[64];
        std::snprintf(metric, sizeof metric, "fill_depth_%u", depth);
        std::snprintf(cond, sizeof cond, "book depth=%u, 1 level consumed; pinned=%d", depth, pinned);
        emit_csv_stats("clob", metric, st, cond);
    }

    // ── 2.5 Per-fill latency and throughput, realistic mix ─────────
    std::printf("\n  2.5 FILL LATENCY + THROUGHPUT (realistic mix, BBO recording ON)\n");
    {
        static CLOBEngine clob;
        optirisk::market::init_clob(clob);

        optirisk::network::BboUpdate* buf = nullptr;
        uint32_t* cnt = nullptr;
        clob.get_write_buffer(&buf, &cnt);

        std::vector<uint64_t> samples;
        samples.reserve(ITERS);

        // Liquidation-shaped order flow: sizes drawn to consume 1-12 levels.
        for (uint64_t i = 0; i < WARMUP; ++i) {
            const uint32_t L = 1 + static_cast<uint32_t>(i % 12);
            auto& b = clob.books[i % 5];
            b.refresh_liquidity(BASE_DEPTH);
            *cnt = 0;
            keep(b.market_sell(units_for_levels(BASE_DEPTH, L), buf, cnt, static_cast<uint8_t>(i % 5)).levels_consumed);
        }

        for (uint64_t i = 0; i < ITERS; ++i) {
            const uint32_t L = 1 + static_cast<uint32_t>(i % 12);
            auto& b = clob.books[i % 5];
            b.refresh_liquidity(BASE_DEPTH);     // UNTIMED reset
            *cnt = 0;

            const uint64_t t0 = read_timestamp();
            const auto fill = b.market_sell(units_for_levels(BASE_DEPTH, L), buf, cnt,
                                            static_cast<uint8_t>(i % 5));
            const uint64_t t1 = read_timestamp();
            keep(fill.levels_consumed);
            samples.push_back(ticks_to_ns(t1 - t0));
        }

        const Stats st = summarize(samples);
        std::snprintf(cond, sizeof cond,
                      "5 books, 1-12 levels per fill, BBO updates recorded, book reset per fill (untimed); pinned=%d",
                      pinned);
        print_stats("2.5a Per-fill latency", st, cond);
        dump_samples(outdir + "/raw_clob_fill.csv", samples);
        emit_csv_stats("clob", "fill_latency_mixed", st, cond);

        // Throughput: the same flow back to back, resets included in the
        // wall time because a real book does have to be re-quoted.
        const uint64_t t0 = read_timestamp();
        for (uint64_t i = 0; i < ITERS; ++i) {
            const uint32_t L = 1 + static_cast<uint32_t>(i % 12);
            auto& b = clob.books[i % 5];
            b.refresh_liquidity(BASE_DEPTH);
            *cnt = 0;
            keep(b.market_sell(units_for_levels(BASE_DEPTH, L), buf, cnt, static_cast<uint8_t>(i % 5)).levels_consumed);
        }
        const uint64_t elapsed = ticks_to_ns(read_timestamp() - t0);
        std::snprintf(cond, sizeof cond, "includes refresh_liquidity re-quote per fill; pinned=%d", pinned);
        print_throughput("2.5b Fill throughput", ITERS, elapsed, cond);
        emit_csv_scalar("clob", "fill_throughput_fps",
                        static_cast<double>(ITERS) / (static_cast<double>(elapsed) / 1e9),
                        "fills/s", cond);
    }

    // ── 2.5c BBO broadcast latency: fill -> visible to reader ──────
    // ── What this measures, and what it does not ───────────────────
    //
    // TRANSPORT: two threads inside ONE process, communicating through the
    // CLOBEngine's ping-pong buffer. The reader spins on an atomic index in
    // the same address space. There is no socket, no second process, and no
    // multicast anywhere in this measurement. It is the cost of one core
    // seeing another core's release-store and reading an already-resident
    // buffer, which is a cross-core cache-line transfer.
    //
    // The shipped system does more than this. main.cpp's broadcast_thread
    // calls clob.get_inactive_read_buffer() exactly as the reader below does,
    // and then hands the span to UdpPublisher::broadcast_bbo(), which issues a
    // sendmsg() to multicast group 239.255.0.1:9090 with a two-entry iovec.
    // That syscall is NOT included here, so do not read this number as
    // "fill to BBO on the wire". Section 2.5d measures the send separately.
    std::printf("\n  2.5c BBO VISIBILITY LATENCY, IN-PROCESS (flip -> other thread observes)\n");
    std::printf("       transport: two threads, one process, shared ping-pong buffer.\n");
    std::printf("       No socket and no multicast in this path. See 2.5d for the send.\n");
    {
        static CLOBEngine clob;
        optirisk::market::init_clob(clob);

        std::vector<uint64_t> samples;
        samples.reserve(BBO_ITERS);

        // Producer stamps a counter value into the first BboUpdate's price
        // field before flipping; the reader converts on observation. Using
        // the buffer itself as the carrier keeps the measurement on the same
        // memory the real broadcast thread reads.
        // Producer and reader are held strictly 1:1 by an ack counter. Without
        // it a fast producer could overwrite `stamp` before the reader read it,
        // and the sample would silently measure the wrong interval.
        std::atomic<uint64_t> stamp{0};
        std::atomic<uint64_t> acked{0};
        std::atomic<bool> stop{false};
        std::atomic<bool> ready{false};
        std::vector<uint64_t> observed;
        observed.reserve(BBO_ITERS);

        std::thread reader([&] {
            pin_to_core(bench_core(1));
            ready.store(true, std::memory_order_release);
            uint8_t last_idx = clob.active_buffer_idx.load(std::memory_order_acquire);
            while (!stop.load(std::memory_order_relaxed)) {
                const uint8_t idx = clob.active_buffer_idx.load(std::memory_order_acquire);
                if (idx == last_idx) {
#if defined(__x86_64__) || defined(_M_X64)
                    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
                    asm volatile("yield" ::: "memory");
#endif
                    continue;
                }
                const uint64_t now = read_timestamp();
                auto span = clob.get_inactive_read_buffer();
                keep(span.size());
                observed.push_back(now - stamp.load(std::memory_order_acquire));
                last_idx = idx;
                acked.fetch_add(1, std::memory_order_release);
            }
        });

        pin_to_core(bench_core(0));
        while (!ready.load(std::memory_order_acquire)) { }

        for (uint64_t i = 0; i < BBO_ITERS; ++i) {
            optirisk::network::BboUpdate* buf = nullptr;
            uint32_t* cnt = nullptr;
            clob.get_write_buffer(&buf, &cnt);
            *cnt = 0;
            clob.books[0].refresh_liquidity(BASE_DEPTH);
            keep(clob.books[0].market_sell(units_for_levels(BASE_DEPTH, 3), buf, cnt, 0).levels_consumed);

            stamp.store(read_timestamp(), std::memory_order_release);
            clob.flip_buffers();

            // Wait for this flip to be acknowledged before producing the next,
            // with a bounded escape so a descheduled reader cannot hang the run.
            const uint64_t want = i + 1;
            const uint64_t deadline = read_timestamp();
            while (acked.load(std::memory_order_acquire) < want) {
                if (ticks_to_ns(read_timestamp() - deadline) > 10'000'000ull) break;  // 10ms
#if defined(__x86_64__) || defined(_M_X64)
                asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
            }
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        for (uint64_t& v : observed) v = ticks_to_ns(v);
        const Stats st = summarize(observed);
        std::snprintf(cond, sizeof cond,
                      "compute stamps then flip_buffers(); reader spins on active_buffer_idx (acquire); pinned=%d",
                      pinned);
        print_stats("2.5c BBO flip -> in-process reader observes", st, cond);
        dump_samples(outdir + "/raw_bbo_publish.csv", observed);
        emit_csv_stats("clob", "bbo_publish_latency", st, cond);
    }

    // ── 2.5d The part 2.5c leaves out: the actual multicast send ───
    std::printf("\n  2.5d BBO MULTICAST SEND (UdpPublisher::broadcast_bbo -> sendmsg)\n");
    {
        static CLOBEngine clob;
        optirisk::market::init_clob(clob);
        static optirisk::network::UdpPublisher udp{"239.255.0.1", 9090};

        optirisk::network::BboUpdate* buf = nullptr;
        uint32_t* cnt = nullptr;
        clob.get_write_buffer(&buf, &cnt);
        *cnt = 0;
        clob.books[0].refresh_liquidity(BASE_DEPTH);
        keep(clob.books[0].market_sell(units_for_levels(BASE_DEPTH, 8), buf, cnt, 0).levels_consumed);
        clob.flip_buffers();

        auto span = clob.get_inactive_read_buffer();
        if (span.empty()) {
            std::printf("       SKIPPED: no BBO updates were recorded to send.\n");
        } else {
            constexpr uint64_t SEND_ITERS = 20'000;
            std::vector<uint64_t> samples;
            samples.reserve(SEND_ITERS);

            for (uint64_t i = 0; i < 1'000; ++i) udp.broadcast_bbo(span);

            for (uint64_t i = 0; i < SEND_ITERS; ++i) {
                const uint64_t t0 = read_timestamp();
                udp.broadcast_bbo(span);
                samples.push_back(ticks_to_ns(read_timestamp() - t0));
            }
            const Stats st = summarize(samples);
            std::snprintf(cond, sizeof cond,
                          "sendmsg to 239.255.0.1:9090, %zu BboUpdate entries (%zu bytes) via 2-entry iovec; pinned=%d",
                          span.size(), span.size() * sizeof(optirisk::network::BboUpdate), pinned);
            print_stats("2.5d sendmsg to multicast group", st, cond);
            dump_samples(outdir + "/raw_bbo_sendmsg.csv", samples);
            emit_csv_stats("clob", "bbo_multicast_sendmsg", st, cond);
            std::printf("       This is the syscall 2.5c omits. Fill-to-wire is 2.5c + 2.5d.\n");
        }
    }

    std::printf("\n");
    return 0;
}
