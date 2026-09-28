#!/usr/bin/env bash
# ============================================================================
# run_all.sh — build and run every OptiRisk benchmark, from scratch, in one
# command. Writes human-readable logs, raw per-sample CSVs, and a combined
# machine-readable CSV into bench/results/.
#
#   ./bench/run_all.sh            # build + run everything
#   ./bench/run_all.sh --quick    # smaller iteration counts, for smoke-testing
#   ./bench/run_all.sh --build-only
#
# Every binary is built twice where prefetching is in play (ON and OFF), so
# the ablation compares two builds of the same source rather than a runtime
# flag the branch predictor could learn.
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SRC="$REPO/backend/src"
OUT="$HERE/results"
BIN="$HERE/bin"

QUICK=0
BUILD_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --quick)      QUICK=1 ;;
        --build-only) BUILD_ONLY=1 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT" "$BIN"

CXX="${CXX:-c++}"

# ── Project Release flags, mirroring backend/CMakeLists.txt ────────────────
ARCH="$(uname -m)"
case "$ARCH" in
    arm64|aarch64) ISA_FLAGS="-march=native" ;;
    x86_64)        ISA_FLAGS="-march=native -mavx2 -mfma" ;;
    *)             ISA_FLAGS="" ;;
esac
# GCC 13+ raises -Winterference-size on disruptor.hpp's use of
# std::hardware_destructive_interference_size, which -Werror turns into a hard
# error. That is a real portability bug in backend/src (the project cannot be
# built with GCC under its own mandated -Werror), but it is not this harness's
# to fix, so suppress just that diagnostic when the compiler understands it.
# Probed rather than assumed: clang errors on unknown -Wno- options under
# -Werror, so a blind add would break the clang build instead.
probe_flag() {
    echo 'int main(){return 0;}' | $CXX -x c++ -std=c++23 -Werror "$1" - -o /dev/null 2>/dev/null
}
EXTRA_FLAGS=""
if probe_flag "-Wno-interference-size"; then
    EXTRA_FLAGS="-Wno-interference-size"
fi

BASE_FLAGS="-std=c++23 -O3 $ISA_FLAGS -Wall -Wextra -Werror $EXTRA_FLAGS -DNDEBUG -I$SRC -I$HERE"
LDFLAGS="-lpthread"

# ══════════════════════════════════════════════════════════════════════════
#  Environment capture — every results file is worthless without this.
# ══════════════════════════════════════════════════════════════════════════
ENVFILE="$OUT/environment.txt"
{
    echo "OptiRisk benchmark environment"
    echo "generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "git commit: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "git dirty: $(test -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" && echo yes || echo no)"
    echo
    echo "── Hardware ──"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo "cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
        echo "arch: $(uname -m)"
        echo "logical cores: $(sysctl -n hw.logicalcpu)"
        echo "physical cores: $(sysctl -n hw.physicalcpu)"
        echo "perf cores: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo n/a)"
        echo "efficiency cores: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo n/a)"
        echo "os: $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
        echo "x86 features: $(sysctl -n machdep.cpu.leaf7_features 2>/dev/null || echo 'none — not an x86 CPU')"
        echo "THREAD PINNING: UNAVAILABLE (macOS has no usable affinity API)"
    else
        echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
        echo "arch: $(uname -m)"
        echo "logical cores: $(nproc)"
        echo "os: $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" || uname -sr)"
        echo "kernel: $(uname -r)"
        echo "avx2: $(grep -qm1 avx2 /proc/cpuinfo && echo yes || echo NO)"
        echo "avx512f: $(grep -qm1 avx512f /proc/cpuinfo && echo yes || echo no)"
        echo "fma: $(grep -qm1 fma /proc/cpuinfo && echo yes || echo NO)"
        echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
        echo "turbo (intel_pstate no_turbo): $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unknown)"
        echo "isolcpus: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo none)"
        echo "THREAD PINNING: AVAILABLE (pthread_setaffinity_np)"
    fi
    echo
    echo "── Toolchain ──"
    echo "compiler: $($CXX --version | head -1)"
    echo "target: $($CXX -dumpmachine 2>/dev/null || echo unknown)"
    echo "flags: $BASE_FLAGS"
    echo "ldflags: $LDFLAGS"
    echo "python: $(python3 --version 2>&1)"
} | tee "$ENVFILE"

# ══════════════════════════════════════════════════════════════════════════
#  Build
# ══════════════════════════════════════════════════════════════════════════
echo
echo "── Building ──────────────────────────────────────────────────────────"

build() {
    local src="$1" out="$2"; shift 2
    printf '  %-34s' "$(basename "$out")"
    if $CXX $BASE_FLAGS "$@" "$src" -o "$out" $LDFLAGS 2> "$OUT/build_$(basename "$out").log"; then
        echo "ok"
    else
        echo "FAILED — see $OUT/build_$(basename "$out").log"
        sed 's/^/      /' "$OUT/build_$(basename "$out").log" | head -30
        return 1
    fi
}

QUICKDEF=""
[[ $QUICK -eq 1 ]] && QUICKDEF="-DOPTIRISK_BENCH_QUICK=1"

FAIL=0
build "$HERE/bench_ring.cpp"         "$BIN/bench_ring"          $QUICKDEF            || FAIL=1
build "$HERE/bench_ring.cpp"         "$BIN/bench_ring_nopf"     $QUICKDEF -DOPTIRISK_NO_PREFETCH || FAIL=1
build "$HERE/bench_cascade.cpp"      "$BIN/bench_cascade"       $QUICKDEF            || FAIL=1
build "$HERE/bench_cascade.cpp"      "$BIN/bench_cascade_nopf"  $QUICKDEF -DOPTIRISK_NO_PREFETCH || FAIL=1
build "$HERE/bench_blackscholes.cpp" "$BIN/bench_blackscholes"  $QUICKDEF            || FAIL=1
build "$HERE/bench_clob.cpp"         "$BIN/bench_clob"          $QUICKDEF            || FAIL=1
build "$HERE/bench_noise.cpp"        "$BIN/bench_noise"         $QUICKDEF            || FAIL=1
build "$HERE/bench_convergence.cpp"  "$BIN/bench_convergence"   $QUICKDEF            || FAIL=1

printf '  %-34s' "disasm_probe.o"
if $CXX $BASE_FLAGS -c "$HERE/disasm_probe.cpp" -o "$BIN/disasm_probe.o" 2> "$OUT/build_disasm.log"; then
    echo "ok"
else
    echo "FAILED — see $OUT/build_disasm.log"
    FAIL=1
fi

if [[ $FAIL -ne 0 ]]; then
    echo
    echo "Build failures above. Aborting."
    exit 1
fi

[[ $BUILD_ONLY -eq 1 ]] && { echo; echo "--build-only: stopping here."; exit 0; }

# ══════════════════════════════════════════════════════════════════════════
#  Step 1.2 — disassemble the Black-Scholes hot loop
# ══════════════════════════════════════════════════════════════════════════
echo
echo "── Step 1.2: Black-Scholes disassembly ───────────────────────────────"
DISASM="$OUT/blackscholes_disasm.txt"
if command -v objdump >/dev/null 2>&1; then
    objdump -d --no-show-raw-insn "$BIN/disasm_probe.o" > "$DISASM" 2>/dev/null
elif command -v otool >/dev/null 2>&1; then
    otool -tvV "$BIN/disasm_probe.o" > "$DISASM" 2>/dev/null
else
    echo "  neither objdump nor otool found — skipping" | tee "$DISASM"
fi

if [[ -s "$DISASM" ]]; then
    echo "  wrote $DISASM"
    case "$ARCH" in
        x86_64) BR='\b(je|jne|jz|jnz|jg|jge|jl|jle|ja|jae|jb|jbe|js|jns)\b' ;;
        *)      BR='\b(b\.[a-z]+|cbz|cbnz|tbz|tbnz)\b' ;;
    esac
    NBR=$(grep -cE "$BR" "$DISASM" || true)
    echo "  conditional branches inside probe_black_scholes: $NBR"
    echo "  (grep the file for the full listing; a nonzero count means the loop"
    echo "   is not branchless even if the arithmetic inside it is)"
    { echo; echo "conditional_branch_count=$NBR"; } >> "$DISASM"
fi

# ══════════════════════════════════════════════════════════════════════════
#  Run
# ══════════════════════════════════════════════════════════════════════════
run() {
    local bin="$1" log="$2"
    echo
    echo "── Running $(basename "$bin") ─────────────────────────────────────"
    if ( cd "$REPO" && "$bin" "$OUT" ) 2>&1 | tee "$OUT/$log"; then :; else
        echo "  (exited nonzero — see $OUT/$log)"
    fi
}

run "$BIN/bench_noise"         "bench_noise.log"   # first: establishes what the tail is worth
run "$BIN/bench_convergence"   "bench_convergence.log"  # correctness before any timing
run "$BIN/bench_ring"          "bench_ring.log"
run "$BIN/bench_ring_nopf"     "bench_ring_noprefetch.log"
run "$BIN/bench_cascade"       "bench_cascade.log"
run "$BIN/bench_cascade_nopf"  "bench_cascade_noprefetch.log"
run "$BIN/bench_blackscholes"  "bench_blackscholes.log"
run "$BIN/bench_clob"          "bench_clob.log"

echo
echo "── Running bench_bridge.py ───────────────────────────────────────────"
if python3 -c "import numpy" 2>/dev/null; then
    python3 "$HERE/bench_bridge.py" 2>&1 | tee "$OUT/bench_bridge.log"
else
    echo "  numpy not installed — skipping the Python side of the handoff bench." \
        | tee "$OUT/bench_bridge.log"
fi

# ══════════════════════════════════════════════════════════════════════════
#  Collate
# ══════════════════════════════════════════════════════════════════════════
echo
echo "── Collating ─────────────────────────────────────────────────────────"
{
    echo "kind,bench,metric,simd,prefetch,count_or_value,mean_ns_or_unit,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,conditions"
    grep -h '^CSV,'    "$OUT"/*.log 2>/dev/null | sed 's/^CSV,/stats,/'
    grep -h '^SCALAR,' "$OUT"/*.log 2>/dev/null | sed 's/^SCALAR,/scalar,/'
} > "$OUT/all_results.csv"
echo "  wrote $OUT/all_results.csv ($(($(wc -l < "$OUT/all_results.csv") - 1)) rows)"
python3 "$HERE/make_report.py" || echo "  (make_report.py failed — all_results.csv is still valid)"

echo
echo "══════════════════════════════════════════════════════════════════════"
echo "  Done. Results in bench/results/"
echo "    environment.txt            hardware + toolchain the numbers came from"
echo "    all_results.csv            every metric, machine-readable"
echo "    bench_*.log                full human-readable output"
echo "    raw_*.csv                  per-sample latencies (the actual evidence)"
echo "    blackscholes_disasm.txt    Step 1.2 disassembly"
echo "  BENCHMARKS.md regenerated at the repo root."
echo "══════════════════════════════════════════════════════════════════════"
