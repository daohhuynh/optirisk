#pragma once
// ============================================================================
// disruptor.hpp — 3-Stage Lock-Free LMAX Disruptor Pipeline
//
// The beating heart of the OptiRisk risk engine. Three threads chase each
// other's atomic cursors through two ring buffers, without EVER touching
// a mutex, condition variable, or any OS-level blocking primitive.
//
// Pipeline topology:
//
//   ┌──────────┐    Ring 1 (ShockPayload)    ┌──────────┐    Ring 2 (TickDelta)
//   ┌───────────┐ │ NETWORK  │ ──────────────────────────► │ COMPUTE  │
//   ──────────────────────► │ BROADCAST │ │ Thread 1 │   write_cursor_1 │
//   Thread 2 │   write_cursor_2        │ Thread 3  │ └──────────┘ read_cursor_1
//   ◄───────    └──────────┘   read_cursor_2 ◄───   └───────────┘
//
// Each thread spins on the PREVIOUS stage's write cursor using
// std::atomic<uint64_t>::load(memory_order_acquire). The producer
// publishes with store(memory_order_release). This is the minimum
// fence cost on both x86-64 (free — TSO gives acquire/release for
// free) and ARM (requires dmb ish only on publish).
//
// Ring buffer size: 1024 slots (power of two → bitmask indexing).
//   sizeof(EventSlot<ShockPayload>) = 64 bytes (one cache line)
//   Total ring memory: 1024 × 64 = 64 KB → fits entirely in L1 cache.
//
// CONSTRAINTS SATISFIED:
//   ✓ Zero dynamic allocation (std::array, no new/malloc)
//   ✓ Zero OS-level blocking (std::atomic only, explicit memory ordering)
//   ✓ Zero dynamic dispatch (no virtual, all templates/constexpr)
//   ✓ Cache-line padded cursors (alignas(64), defeating false sharing)
//   ✓ Cache-line padded slots (each slot = 1 cache line exactly)
// ============================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "network/wire_protocol.hpp"

namespace optirisk::concurrency {

// ── Cache-Line Size ────────────────────────────────────────────────
// Fixed at 64 rather than std::hardware_destructive_interference_size.
// GCC raises -Winterference-size on that constant because its value can
// change with -mtune and is therefore ABI-unstable; combined with this
// project's -Werror it is a hard build failure, so the backend would not
// compile with GCC at all. 64 is correct for every x86-64 part and for
// Apple Silicon, and a padding constant that silently varies between
// compilers is a worse foundation for a lock-free ring buffer than one
// that is simply stated.
inline constexpr std::size_t CACHE_LINE = 64;

// ── Ring Buffer Configuration ──────────────────────────────────────
inline constexpr std::size_t RING_SIZE = 1024;
inline constexpr std::size_t RING_MASK = RING_SIZE - 1;

static_assert((RING_SIZE & (RING_SIZE - 1)) == 0,
              "RING_SIZE must be a power of two for bitmask indexing");

// ── EventSlot ──────────────────────────────────────────────────────
//
// Each slot in the ring buffer occupies exactly ONE cache line (64 bytes).
// This prevents false sharing between adjacent slots that may be written
// by the producer and read by the consumer simultaneously.
//
// Layout for EventSlot<ShockPayload>:
//   [0..55]  payload (ShockPayload = 56 bytes)
//   [56..63] sequence (int64_t = 8 bytes)
//   Total: 64 bytes = 1 cache line ✓
//
// Layout for EventSlot<TickDelta>:
//   [0..47]  payload (TickDelta = 48 bytes)
//   [48..55] sequence (int64_t = 8 bytes)
//   [56..63] padding (8 bytes)
//   Total: 64 bytes = 1 cache line ✓
//
template <typename T> struct alignas(CACHE_LINE) EventSlot {
  T payload{};
  int64_t sequence = -1; // Published sequence number for this slot
  // Implicit padding to CACHE_LINE via alignas

  // Verify we fit in a single cache line
  static_assert(sizeof(T) + sizeof(int64_t) <= CACHE_LINE,
                "Payload + sequence must fit within one cache line");
};

static_assert(sizeof(EventSlot<optirisk::network::ShockPayload>) == CACHE_LINE,
              "EventSlot<ShockPayload> must be exactly one cache line");
static_assert(sizeof(EventSlot<optirisk::network::TickDelta>) == CACHE_LINE,
              "EventSlot<TickDelta> must be exactly one cache line");

// ── Padded Cursor ──────────────────────────────────────────────────
//
// Each cursor occupies its OWN cache line to prevent false sharing.
// The Network thread's write cursor and the Compute thread's read
// cursor would otherwise share a cache line, causing the cache
// coherency protocol (MESI/MOESI) to bounce the line between cores
// on every atomic store — destroying throughput.
//
// On x86-64 with TSO: load(acquire) is free, store(release) is free.
// On ARM64 (Apple Silicon): load(acquire) = ldar, store(release) = stlr.
// Sequential consistency (seq_cst) is NEVER used — it forces full
// memory barriers (mfence on x86, dmb ish on ARM) on every operation.
//
struct alignas(CACHE_LINE) PaddedCursor {
  std::atomic<uint64_t> value{0};
  // Explicit padding to fill the cache line
  char _pad[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
};

static_assert(sizeof(PaddedCursor) == CACHE_LINE,
              "PaddedCursor must occupy exactly one cache line");
static_assert(alignof(PaddedCursor) == CACHE_LINE,
              "PaddedCursor must be aligned to cache line boundary");

// ── RingBuffer ─────────────────────────────────────────────────────
//
// Single-producer, single-consumer (SPSC) lock-free ring buffer.
// The producer claims a slot, writes the payload, then publishes it
// by storing the sequence number with release semantics. The consumer
// spins on the cursor with acquire semantics until data is available.
//
// Back-pressure: if the producer laps the consumer (producer is 1024
// slots ahead), the producer spin-waits until the consumer catches up.
// This prevents unbounded memory growth and ensures the ring never
// overwrites unconsumed data.
//
template <typename T> class RingBuffer {
public:
  // ── Producer API ───────────────────────────────────────────────

  // Claim the next writable slot. Returns the sequence number, or UINT64_MAX on
  // shutdown. Will spin-wait if the ring is full, but monitors the is_running
  // flag to prevent livelocks.
  [[nodiscard]] uint64_t claim(const PaddedCursor &consumer_cursor,
                               const std::atomic<bool> &is_running) noexcept {
    const uint64_t seq = write_cursor_.value.load(std::memory_order_relaxed);

    // Back-pressure: spin until consumer has freed a slot.
    if (seq >= RING_SIZE) [[likely]] {
      const uint64_t wrap_point = seq - RING_SIZE;
      while (consumer_cursor.value.load(std::memory_order_acquire) <=
             wrap_point) [[unlikely]] {
        // THE FIX: Check the shutdown flag inside the starvation loop
        if (!is_running.load(std::memory_order_acquire)) [[unlikely]] {
          return UINT64_MAX; // Poison pill signal to abort
        }
        cpu_pause();
      }
    }
    return seq;
  }

  // Get a mutable reference to the slot's payload at `seq`.
  [[nodiscard]] T &get(uint64_t seq) noexcept {
    return slots_[seq & RING_MASK].payload;
  }

  // Get a const reference to the slot's payload at `seq`.
  [[nodiscard]] const T &get(uint64_t seq) const noexcept {
    return slots_[seq & RING_MASK].payload;
  }

  // Publish the slot, making it visible to the consumer.
  // Stores the sequence into the slot first (for per-slot validation),
  // then advances the write cursor with release semantics so the
  // consumer's acquire-load sees the fully written payload.
  void publish(uint64_t seq) noexcept {
    slots_[seq & RING_MASK].sequence = static_cast<int64_t>(seq);
    write_cursor_.value.store(seq + 1, std::memory_order_release);
  }

  // ── Consumer API ───────────────────────────────────────────────

  // Try to read the slot at `expected_seq`.
  // Returns true if data is available (caller should read via get()),
  // false if the consumer has caught up to the producer.
  [[nodiscard]] bool available(uint64_t expected_seq) const noexcept {
    return write_cursor_.value.load(std::memory_order_acquire) > expected_seq;
  }

  // ── Cursor Access ──────────────────────────────────────────────

  [[nodiscard]] const PaddedCursor &write_cursor() const noexcept {
    return write_cursor_;
  }

  [[nodiscard]] uint64_t published() const noexcept {
    return write_cursor_.value.load(std::memory_order_acquire);
  }

private:
  // ── Data Layout ────────────────────────────────────────────────
  // The slot array is first (64 KB), followed by the write cursor
  // on its own cache line. This ordering ensures the write cursor
  // doesn't share a page with the first few slots.
  alignas(CACHE_LINE) std::array<EventSlot<T>, RING_SIZE> slots_{};
  PaddedCursor write_cursor_{};

  // ── CPU Pause Hint ─────────────────────────────────────────────
  // Reduces power consumption and frees execution resources for
  // the sibling hyperthread during spin-wait loops.
  static void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
  }
};

// ── DisruptorEngine ────────────────────────────────────────────────
//
// Complete 3-stage pipeline engine. Owns both ring buffers and all
// cursor state. Provides the thread entry points that main.cpp
// launches. The entire struct is declared `static` to place it in
// .bss — zero-initialized by the OS loader, no runtime memset.
//
// Memory budget:
//   Ring 1 (ShockPayload):  1024 × 64 + 64 = 65,600 bytes
//   Ring 2 (TickDelta):     1024 × 64 + 64 = 65,600 bytes
//   Cursors:                3 × 64         =    192 bytes
//   Total:                                 ≈ 131 KB
//   Fits comfortably in L2 cache.
//
// Thread safety contract:
//   - network_cursor_:   written by Network,  read by Compute
//   - compute_cursor_:   written by Compute,  read by Broadcast
//   - broadcast_cursor_: written by Broadcast, read by Network (back-pressure)
//
struct DisruptorEngine {
  // ── Ring Buffers ───────────────────────────────────────────────
  RingBuffer<optirisk::network::ShockPayload> shock_ring; // Network → Compute
  RingBuffer<optirisk::network::TickDelta> tick_ring;     // Compute → Broadcast

  // ── Consumer Cursors (one per stage) ───────────────────────────
  // Each cursor tracks the LAST CONSUMED sequence for that stage.
  // Written exclusively by its owner thread, read by the upstream
  // producer for back-pressure coordination.
  PaddedCursor network_cursor{}; // Network writes, Compute reads (back-pressure
                                 // on shock_ring)
  PaddedCursor compute_cursor{}; // Compute writes, Broadcast reads
                                 // (back-pressure on tick_ring)
  PaddedCursor broadcast_cursor{}; // Broadcast writes, Network reads
                                   // (end-to-end back-pressure)

  // ── Batch Counters (per-stage diagnostics) ─────────────────────
  // Only written by the owning thread, read for printf — no atomics needed.
  uint64_t network_count = 0;
  uint64_t compute_count = 0;
  uint64_t broadcast_count = 0;
};

static_assert(sizeof(PaddedCursor) == CACHE_LINE);

// ── Pre-configured pipeline size (kept for backward compat) ────────
inline constexpr std::size_t PIPELINE_RING_SIZE = RING_SIZE;

} // namespace optirisk::concurrency
