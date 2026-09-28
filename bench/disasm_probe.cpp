// ============================================================================
// disasm_probe.cpp — Step 1.2: give the "branchless" claim something to
// disassemble.
//
// compute_options_m2m() is __attribute__((always_inline)), so it emits no
// symbol of its own and cannot be objdump'd directly. This file wraps it in a
// noinline function with an unmangled name so `objdump -d --disassemble=probe_*`
// shows exactly the code the kernel generates, with nothing else in the frame.
//
// Built by run_all.sh with the project's Release flags. Not linked into
// anything that runs.
// ============================================================================

#include <array>
#include <cstdint>

#include "compute/black_scholes_simd.hpp"
#include "memory/options_book.hpp"

extern "C" {

// The option kernel — the subject of the branchless claim.
__attribute__((noinline, used))
void probe_black_scholes(optirisk::memory::OptionsBook* book,
                         float underlying,
                         uint32_t count,
                         float* out) {
    optirisk::compute::compute_options_m2m(book, underlying, count, out);
}

}  // extern "C"
