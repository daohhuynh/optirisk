#!/usr/bin/env python3
"""
ablation_report.py — decompose defaults into contagion channels.

Reads the four bench_ablation logs and computes, per scenario, a 2x2 factorial
decomposition of the default count:

    baseline     defaults with BOTH channels off. These firms fail on the
                 direct mark-to-market of the shock alone and belong to
                 neither channel.
    gamma        (no-counterparty) - baseline
                 what the price-feedback channel adds when it is the only
                 channel running.
    counterparty (no-gamma) - baseline
                 what the network channel adds when it is the only one.
    interaction  full - baseline - gamma - counterparty

The interaction term is the point of the exercise. If it is zero the channels
are additive and "share of defaults" is meaningful. If it is not, the shares do
not add up and saying "X% of defaults come from channel Y" is wrong, because
some defaults require both channels to be present at once.
"""

import os
import re
import sys

VARIANTS = ["full", "no-gamma", "no-counterparty", "neither"]
LOGS = {
    "full": "bench_ablation_full.log",
    "no-gamma": "bench_ablation_nogamma.log",
    "no-counterparty": "bench_ablation_nocp.log",
    "neither": "bench_ablation_neither.log",
}


def parse(results_dir):
    """defaults[variant][scenario] = (count, last_round, truncated_at_1024)"""
    defaults, last_round, truncated = {}, {}, {}
    for variant, fname in LOGS.items():
        path = os.path.join(results_dir, fname)
        defaults[variant], last_round[variant], truncated[variant] = {}, {}, {}
        if not os.path.exists(path):
            continue
        with open(path) as f:
            for line in f:
                if not line.startswith("SCALAR,ablation,"):
                    continue
                parts = line.rstrip("\n").split(",", 7)
                if len(parts) < 8:
                    continue
                metric, value, cond = parts[2], parts[5], parts[7]
                if "|" not in metric:
                    continue
                bits = metric.split("|")
                scen = bits[1]
                if len(bits) == 3 and bits[2] == "last_round":
                    last_round[variant][scen] = int(float(value))
                elif len(bits) == 2:
                    defaults[variant][scen] = int(float(value))
                    truncated[variant][scen] = "TRUNCATED" in cond
    return defaults, last_round, truncated


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "results"
    d, lr, trunc = parse(results_dir)

    if not d.get("full"):
        print("  no ablation results found; skipping decomposition")
        return 0

    scenarios = list(d["full"].keys())

    print()
    print("=" * 100)
    print("  CONTAGION CHANNEL ABLATION")
    print("=" * 100)

    print("\n  TOTAL DEFAULTS BY VARIANT (out of 500 nodes)\n")
    hdr = f"  {'scenario':<20}" + "".join(f"{v:>18}" for v in VARIANTS)
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for s in scenarios:
        row = f"  {s:<20}"
        for v in VARIANTS:
            row += f"{d[v].get(s, '-'):>18}"
        print(row)

    print("\n  ROUND OF LAST DEFAULT (-1 = no defaults)\n")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    for s in scenarios:
        row = f"  {s:<20}"
        for v in VARIANTS:
            row += f"{lr[v].get(s, '-'):>18}"
        print(row)

    print("\n  DECOMPOSITION: where each default comes from\n")
    print(f"  {'scenario':<20}{'full':>7}{'baseline':>10}{'gamma':>8}{'counterp':>10}"
          f"{'interact':>10}   {'additive?':<12}")
    print("  " + "-" * 88)

    any_interaction = False
    for s in scenarios:
        full = d["full"].get(s)
        base = d["neither"].get(s)
        nog = d["no-gamma"].get(s)
        nocp = d["no-counterparty"].get(s)
        if None in (full, base, nog, nocp):
            continue
        gamma = nocp - base          # only the price channel was live
        cp = nog - base              # only the network channel was live
        inter = full - base - gamma - cp
        if inter != 0:
            any_interaction = True
        verdict = "additive" if inter == 0 else f"NOT additive"
        print(f"  {s:<20}{full:>7}{base:>10}{gamma:>8}{cp:>10}{inter:>10}   {verdict:<12}")

    print("\n  SHARE OF DEFAULTS BY CHANNEL\n")
    print(f"  {'scenario':<20}{'baseline':>10}{'gamma':>9}{'counterp':>10}{'interact':>10}")
    print("  " + "-" * 60)
    for s in scenarios:
        full = d["full"].get(s)
        base = d["neither"].get(s)
        nog = d["no-gamma"].get(s)
        nocp = d["no-counterparty"].get(s)
        if None in (full, base, nog, nocp) or full == 0:
            print(f"  {s:<20}{'(no defaults)':>39}")
            continue
        gamma = nocp - base
        cp = nog - base
        inter = full - base - gamma - cp
        print(f"  {s:<20}{base/full*100:>9.1f}%{gamma/full*100:>8.1f}%"
              f"{cp/full*100:>9.1f}%{inter/full*100:>9.1f}%")

    print("\n  1024-ROUND CAP: was it truncating?\n")
    for v in VARIANTS:
        hit = [s for s in scenarios if trunc[v].get(s)]
        if hit:
            print(f"    {v:<18} TRUNCATED: {', '.join(hit)}")
        else:
            print(f"    {v:<18} adequate for every scenario")

    if any_interaction:
        print("\n  *** CHANNELS INTERACT. The shares above do not add to 100% by")
        print("      construction, and no single channel can be credited with a")
        print("      fixed percentage of defaults: some firms fail only when both")
        print("      channels are present. Quote the decomposition, not a share. ***")
    else:
        print("\n  Channels are additive across every scenario; shares are meaningful.")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
