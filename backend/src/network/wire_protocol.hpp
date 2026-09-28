#pragma once
// ============================================================================
// wire_protocol.hpp — OptiRisk Binary Wire Protocol
//
// Defines the exact byte layout of every message between frontend ↔ backend.
// ALL structs use #pragma pack(push, 1) to guarantee zero compiler padding.
// ALL fields use fixed-width integer types for cross-platform determinism.
//
// Protocol overview:
//   Frontend → Backend: ShockPayload  (user injects a stress-test shock)
//   Backend → Frontend: TickDelta     (per-tick risk state delta broadcast)
//   Backend → Frontend: NodeSnapshot  (full node state, defined in csr_graph.hpp)
//   Backend → Frontend: MarketAnchors (sent once at connection handshake)
//
// Every message is prefixed by a 4-byte MessageHeader so the receiver can
// disambiguate types without any string parsing or JSON.
//
// Wire encoding: little-endian (x86-64 native). Frontend uses DataView
// with littleEndian=true on typed arrays.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace optirisk::network {

// ── Message Type Tags ─────────────────────────────────────────────
// Interned as uint8_t — no string comparisons on the hot path.
enum class MsgType : uint8_t {
    ShockPayload   = 0x01,  // Frontend → Backend
    TickDelta      = 0x02,  // Backend → Frontend (per-tick delta)
    NodeSnapshot   = 0x03,  // Backend → Frontend (full node state)
    MarketAnchors  = 0x04,  // Backend → Frontend (handshake)
    CreditRevoke   = 0x05,  // Frontend → Backend (Shield Action)
    ShortOrder     = 0x06,  // Frontend → Backend (Sword Action)
    VaRReport      = 0x07,  // Backend → Frontend (Monte Carlo results)
    BboChange      = 0x08,  // Backend → Frontend (ITCH limit order changes)
    Heartbeat      = 0xFE,  // Bidirectional keepalive
    Error          = 0xFF,  // Backend → Frontend (error notification)
};

// ── Message Header (prefixes every frame) ─────────────────────────
// 4 bytes. The receiver reads this first to determine how many bytes
// to consume from the stream.
//
// Wire layout:
//   [0]     msg_type    MsgType (uint8_t)
//   [1]     _reserved   uint8_t (zero, future flags)
//   [2..3]  payload_len uint16_t (body size in bytes, excludes header)
//
#pragma pack(push, 1)
struct MessageHeader {
    MsgType  msg_type;
    uint8_t  _reserved = 0;
    uint16_t payload_len;
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 4,
              "MessageHeader must be exactly 4 bytes");

// ============================================================================
// FRONTEND → BACKEND: ShockPayload
// ============================================================================
//
// Sent when the user injects a stress-test scenario from the UI.
// The user picks a target node (or 0xFFFFFFFF for "market-wide") and
// specifies the magnitude of the shock across each asset class.
//
// Wire layout (52 bytes):
//   [0..3]   target_node_id  uint32_t  (0xFFFFFFFF = broadcast to all)
//   [4..7]   shock_type      uint32_t  (enum ShockType as uint32_t)
//   [8..15]  equities_delta  double    (fractional change, e.g. -0.30 = -30%)
//   [16..23] real_estate_delta double
//   [24..31] crypto_delta    double
//   [32..39] treasuries_delta double
//   [40..47] corp_bonds_delta double
//   [48..55] timestamp_ns    uint64_t  (frontend monotonic clock)
//
#pragma pack(push, 1)
struct ShockPayload {
    uint32_t target_node_id;     // Target node, or 0xFFFFFFFF for market-wide
    uint32_t shock_type;         // 0=custom, 1=lehman, 2=covid, 3=rate_hike, 4=crypto_crash
    double   equities_delta;     // Fractional Δ applied to equities exposure
    double   real_estate_delta;  // Fractional Δ applied to real estate exposure
    double   crypto_delta;       // Fractional Δ applied to crypto exposure
    double   treasuries_delta;   // Fractional Δ applied to treasuries exposure
    double   corp_bonds_delta;   // Fractional Δ applied to corp bonds exposure
    uint64_t timestamp_ns;       // Frontend monotonic clock (for RTT measurement)
};
#pragma pack(pop)

static_assert(sizeof(ShockPayload) == 56,
              "ShockPayload must be exactly 56 bytes");
static_assert(std::is_trivially_copyable_v<ShockPayload>,
              "ShockPayload must be trivially copyable for memcpy");

// ── Pre-defined Shock Scenario Tags ────────────────────────────────
// These match shock_type field values. The compute thread maps these
// to pre-baked delta vectors at compile time.
enum class ShockType : uint32_t {
    Custom      = 0,
    Lehman2008  = 1,  // Equity -40%, RE -25%, Corp Bonds -15%
    Covid2020   = 2,  // Equity -35%, RE -10%, Crypto -50%
    RateHike    = 3,  // Treasuries -20%, Corp Bonds -12%
    CryptoCrash = 4,  // Crypto -80%, Equity -5%
};

// ============================================================================
// BACKEND → FRONTEND: TickDelta
// ============================================================================
//
// Broadcast once per compute tick to all connected clients. Contains
// only the fields that changed — NOT a full snapshot. The frontend
// applies these deltas to its local state buffer.
//
// Wire layout (56 bytes):
//   [0..3]   node_id         uint32_t
//   [4..7]   risk_score      float     (updated risk ∈ [0.0, 1.0])
//   [8..15]  nav             double    (updated NAV)
//   [16..23] exposure_total  double    (sum of all 5 asset exposures)
//   [24..31] delta_nav       double    (Δ from previous tick)
//   [32..39] delta_exposure  double    (Δ from previous tick)
//   [40]     is_defaulted    uint8_t   (1 = default event this tick)
//   [41]     hub_id          uint8_t
//   [42]     cascade_depth   uint8_t   (BFS depth if part of cascade, else 0)
//   [43]     _pad            uint8_t   (alignment padding, zero)
//   [44..47] tick_seq        uint32_t  (monotonic tick counter)
//   [48..55] compute_ns      uint64_t  (nanoseconds in the compute kernel)
//
#pragma pack(push, 1)
struct TickDelta {
    uint32_t node_id;
    float    risk_score;
    double   nav;
    double   exposure_total;
    double   delta_nav;
    double   delta_exposure;
    uint8_t  is_defaulted;
    uint8_t  hub_id;
    uint8_t  cascade_depth;
    uint8_t  _pad = 0;
    uint32_t tick_seq;
    uint64_t compute_ns;       // Nanoseconds spent in the compute kernel (see ticks_to_ns)
};

// ── CreditRevoke (16 bytes) ───────────────────────────────────────
// The Shield mechanic: a player revokes credit to an infectious node.
struct CreditRevoke {
    uint32_t source_node;    // The node whose credit line to sever
    uint32_t target_node;    // The creditor node
    uint64_t timestamp_ns;
};

// ── ShortOrder (24 bytes) ─────────────────────────────────────────
// The Sword mechanic: a player shorts an asset inside a dying node.
struct ShortOrder {
    uint32_t target_node;
    uint8_t  asset_class;    // AssetClass enum (0-4)
    uint8_t  _pad[3];
    double   quantity;
    uint64_t timestamp_ns;
};

// ── VaRReport (16 bytes) ──────────────────────────────────────────
// Sent after a Monte Carlo burst: gives the P95 VaR risk for a node.
struct VaRReport {
    uint32_t target_node;
    uint32_t paths_run;
    double   var_95;         // USD worst-case drop at 95% confidence
};

#pragma pack(pop)

static_assert(sizeof(TickDelta) == 56,
              "TickDelta must be exactly 56 bytes");
static_assert(std::is_trivially_copyable_v<TickDelta>,
              "TickDelta must be trivially copyable for memcpy");

static_assert(sizeof(CreditRevoke) == 16, "CreditRevoke must be exactly 16 bytes");
static_assert(sizeof(ShortOrder) == 24, "ShortOrder must be exactly 24 bytes");
static_assert(sizeof(VaRReport) == 16, "VaRReport must be exactly 16 bytes");

// ============================================================================
// Serialization Helpers
// ============================================================================
//
// These are zero-copy casts. The caller owns the buffer; we just
// reinterpret the bytes. No allocation, no parsing.
//

// Deserialize a raw byte buffer into a ShockPayload.
// Returns true if the buffer is the correct size, false otherwise.
[[nodiscard]] inline bool parse_shock(const void* buf, std::size_t len,
                                      ShockPayload& out) noexcept {
    if (len < sizeof(ShockPayload)) [[unlikely]] return false;
    std::memcpy(&out, buf, sizeof(ShockPayload));
    return true;
}

// Serialize a TickDelta into a caller-provided byte buffer.
// Buffer must be at least sizeof(MessageHeader) + sizeof(TickDelta) bytes.
// Returns total bytes written.
[[nodiscard]] inline std::size_t serialize_tick(const TickDelta& tick,
                                                void* buf,
                                                std::size_t buf_len) noexcept {
    constexpr std::size_t TOTAL = sizeof(MessageHeader) + sizeof(TickDelta);
    if (buf_len < TOTAL) [[unlikely]] return 0;

    MessageHeader hdr{};
    hdr.msg_type    = MsgType::TickDelta;
    hdr.payload_len = sizeof(TickDelta);

    auto* dst = static_cast<uint8_t*>(buf);
    std::memcpy(dst, &hdr, sizeof(MessageHeader));
    std::memcpy(dst + sizeof(MessageHeader), &tick, sizeof(TickDelta));
    return TOTAL;
}

// Serialize a ShockPayload into a caller-provided byte buffer (for testing).
[[nodiscard]] inline std::size_t serialize_shock(const ShockPayload& shock,
                                                  void* buf,
                                                  std::size_t buf_len) noexcept {
    constexpr std::size_t TOTAL = sizeof(MessageHeader) + sizeof(ShockPayload);
    if (buf_len < TOTAL) [[unlikely]] return 0;

    MessageHeader hdr{};
    hdr.msg_type    = MsgType::ShockPayload;
    hdr.payload_len = sizeof(ShockPayload);

    auto* dst = static_cast<uint8_t*>(buf);
    std::memcpy(dst, &hdr, sizeof(MessageHeader));
    std::memcpy(dst + sizeof(MessageHeader), &shock, sizeof(ShockPayload));
    return TOTAL;
}

} // namespace optirisk::network
