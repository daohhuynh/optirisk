// ============================================================================
// test_wire_protocol.cpp — Wire Protocol Struct Validation
//
// Standalone test binary. No test framework, no heap allocation.
// Validates:
//   1. Struct sizes match static_assert expectations
//   2. Round-trip serialize → parse preserves all fields
//   3. Byte offsets match the documented wire layout
//
// Build:
//   g++ -std=c++23 -O3 -Wall -Wextra -Werror -I src \
//       src/network/test_wire_protocol.cpp -o build/test_wire_protocol
// ============================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cmath>

#include "network/wire_protocol.hpp"

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

// ── Test 1: Struct Sizes ──────────────────────────────────────────
static void test_struct_sizes() {
    std::printf("[test] struct sizes\n");
    CHECK(sizeof(MessageHeader) == 4,   "MessageHeader == 4 bytes");
    CHECK(sizeof(ShockPayload)  == 56,  "ShockPayload  == 56 bytes");
    CHECK(sizeof(TickDelta)     == 56,  "TickDelta      == 56 bytes");
    std::printf("  MessageHeader: %zu bytes\n", sizeof(MessageHeader));
    std::printf("  ShockPayload:  %zu bytes\n", sizeof(ShockPayload));
    std::printf("  TickDelta:     %zu bytes\n", sizeof(TickDelta));
}

// ── Test 2: ShockPayload Byte Offsets ─────────────────────────────
static void test_shock_offsets() {
    std::printf("[test] ShockPayload byte offsets\n");
    CHECK(offsetof(ShockPayload, target_node_id)    ==  0, "target_node_id @ 0");
    CHECK(offsetof(ShockPayload, shock_type)        ==  4, "shock_type @ 4");
    CHECK(offsetof(ShockPayload, equities_delta)    ==  8, "equities_delta @ 8");
    CHECK(offsetof(ShockPayload, real_estate_delta) == 16, "real_estate_delta @ 16");
    CHECK(offsetof(ShockPayload, crypto_delta)      == 24, "crypto_delta @ 24");
    CHECK(offsetof(ShockPayload, treasuries_delta)  == 32, "treasuries_delta @ 32");
    CHECK(offsetof(ShockPayload, corp_bonds_delta)  == 40, "corp_bonds_delta @ 40");
    CHECK(offsetof(ShockPayload, timestamp_ns)      == 48, "timestamp_ns @ 48");
}

// ── Test 3: TickDelta Byte Offsets ────────────────────────────────
static void test_tick_offsets() {
    std::printf("[test] TickDelta byte offsets\n");
    CHECK(offsetof(TickDelta, node_id)         ==  0, "node_id @ 0");
    CHECK(offsetof(TickDelta, risk_score)      ==  4, "risk_score @ 4");
    CHECK(offsetof(TickDelta, nav)             ==  8, "nav @ 8");
    CHECK(offsetof(TickDelta, exposure_total)  == 16, "exposure_total @ 16");
    CHECK(offsetof(TickDelta, delta_nav)       == 24, "delta_nav @ 24");
    CHECK(offsetof(TickDelta, delta_exposure)  == 32, "delta_exposure @ 32");
    CHECK(offsetof(TickDelta, is_defaulted)    == 40, "is_defaulted @ 40");
    CHECK(offsetof(TickDelta, hub_id)          == 41, "hub_id @ 41");
    CHECK(offsetof(TickDelta, cascade_depth)   == 42, "cascade_depth @ 42");
    CHECK(offsetof(TickDelta, _pad)            == 43, "_pad @ 43");
    CHECK(offsetof(TickDelta, tick_seq)        == 44, "tick_seq @ 44");
    CHECK(offsetof(TickDelta, compute_ns)      == 48, "compute_ns @ 48");
}

// ── Test 4: ShockPayload Round-Trip ───────────────────────────────
static void test_shock_roundtrip() {
    std::printf("[test] ShockPayload serialize → parse round-trip\n");

    ShockPayload original{};
    original.target_node_id    = 42;
    original.shock_type        = static_cast<uint32_t>(ShockType::Lehman2008);
    original.equities_delta    = -0.40;
    original.real_estate_delta = -0.25;
    original.crypto_delta      = -0.10;
    original.treasuries_delta  = 0.05;
    original.corp_bonds_delta  = -0.15;
    original.timestamp_ns      = 1234567890123456789ULL;

    // Serialize
    uint8_t buf[128];
    std::size_t n = serialize_shock(original, buf, sizeof(buf));
    CHECK(n == sizeof(MessageHeader) + sizeof(ShockPayload),
          "serialize_shock returns correct size");

    // Verify header
    MessageHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(MessageHeader));
    CHECK(hdr.msg_type    == MsgType::ShockPayload, "header msg_type == ShockPayload");
    CHECK(hdr.payload_len == sizeof(ShockPayload),  "header payload_len matches");

    // Parse back
    ShockPayload parsed{};
    bool ok = parse_shock(buf + sizeof(MessageHeader), hdr.payload_len, parsed);
    CHECK(ok, "parse_shock succeeds");
    CHECK(parsed.target_node_id    == 42,                  "target_node_id preserved");
    CHECK(parsed.shock_type        == 1,                   "shock_type preserved");
    CHECK(std::fabs(parsed.equities_delta - (-0.40)) < 1e-12, "equities_delta preserved");
    CHECK(std::fabs(parsed.real_estate_delta - (-0.25)) < 1e-12, "real_estate_delta preserved");
    CHECK(std::fabs(parsed.crypto_delta - (-0.10)) < 1e-12, "crypto_delta preserved");
    CHECK(std::fabs(parsed.treasuries_delta - 0.05) < 1e-12, "treasuries_delta preserved");
    CHECK(std::fabs(parsed.corp_bonds_delta - (-0.15)) < 1e-12, "corp_bonds_delta preserved");
    CHECK(parsed.timestamp_ns == 1234567890123456789ULL, "timestamp_ns preserved");
}

// ── Test 5: TickDelta Round-Trip ──────────────────────────────────
static void test_tick_roundtrip() {
    std::printf("[test] TickDelta serialize → parse round-trip\n");

    TickDelta original{};
    original.node_id        = 255;
    original.risk_score     = 0.87f;
    original.nav            = 1500000000.50;
    original.exposure_total = 750000000.25;
    original.delta_nav      = -5000000.10;
    original.delta_exposure = 2500000.05;
    original.is_defaulted   = 1;
    original.hub_id         = 3;
    original.cascade_depth  = 2;
    original.tick_seq       = 999999;
    original.compute_ns     = 42424242ULL;

    // Serialize
    uint8_t buf[128];
    std::size_t n = serialize_tick(original, buf, sizeof(buf));
    CHECK(n == sizeof(MessageHeader) + sizeof(TickDelta),
          "serialize_tick returns correct size");

    // Verify header
    MessageHeader hdr{};
    std::memcpy(&hdr, buf, sizeof(MessageHeader));
    CHECK(hdr.msg_type    == MsgType::TickDelta,   "header msg_type == TickDelta");
    CHECK(hdr.payload_len == sizeof(TickDelta),    "header payload_len matches");

    // Parse back (memcpy from payload region)
    TickDelta parsed{};
    std::memcpy(&parsed, buf + sizeof(MessageHeader), sizeof(TickDelta));
    CHECK(parsed.node_id        == 255,     "node_id preserved");
    CHECK(std::fabs(parsed.risk_score - 0.87f) < 1e-6f, "risk_score preserved");
    CHECK(std::fabs(parsed.nav - 1500000000.50) < 1e-6, "nav preserved");
    CHECK(std::fabs(parsed.exposure_total - 750000000.25) < 1e-6, "exposure_total preserved");
    CHECK(std::fabs(parsed.delta_nav - (-5000000.10)) < 1e-6, "delta_nav preserved");
    CHECK(std::fabs(parsed.delta_exposure - 2500000.05) < 1e-6, "delta_exposure preserved");
    CHECK(parsed.is_defaulted   == 1,       "is_defaulted preserved");
    CHECK(parsed.hub_id         == 3,       "hub_id preserved");
    CHECK(parsed.cascade_depth  == 2,       "cascade_depth preserved");
    CHECK(parsed.tick_seq       == 999999,  "tick_seq preserved");
    CHECK(parsed.compute_ns     == 42424242ULL, "compute_ns preserved");
}

// ── Test 6: Buffer Too Small ──────────────────────────────────────
static void test_undersized_buffer() {
    std::printf("[test] undersized buffer rejection\n");

    ShockPayload shock{};
    uint8_t tiny_buf[4];
    std::size_t n = serialize_shock(shock, tiny_buf, sizeof(tiny_buf));
    CHECK(n == 0, "serialize_shock returns 0 for undersized buffer");

    bool ok = parse_shock(tiny_buf, 2, shock);
    CHECK(!ok, "parse_shock returns false for undersized input");
}

int main() {
    std::printf("═══════════════════════════════════════════\n");
    std::printf("  OptiRisk Wire Protocol Tests\n");
    std::printf("═══════════════════════════════════════════\n\n");

    test_struct_sizes();
    test_shock_offsets();
    test_tick_offsets();
    test_shock_roundtrip();
    test_tick_roundtrip();
    test_undersized_buffer();

    std::printf("\n═══════════════════════════════════════════\n");
    std::printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    std::printf("═══════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
