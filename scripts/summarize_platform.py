#!/usr/bin/env python3
"""Summarise one platform's benchmark results into SUMMARY.md (the files are generated, never typed).

Reads the layout written by scripts/bench_macos.sh and scripts/bench_windows.ps1:

  <dir>/single/final_<workload>.json      lob_bench          (single-thread engine)
  <dir>/mt/<queue>[-nocb]_<workload>.json lob_mt_bench       (producer -> queue -> matching thread)
  <dir>/realdata/bench.json               lob_itch bench     (optional: real Nasdaq ITCH replay)

  python scripts/summarize_platform.py bench/results/platforms/macos-apple-m3
"""
import glob
import json
import os
import sys

WORKLOADS = ["passive", "aggressive", "balanced"]


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def med(d):
    return d["median"]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    single = {w: load(p) for w in WORKLOADS if os.path.exists(p := os.path.join(root, "single", f"final_{w}.json"))}
    mt = {}
    for p in sorted(glob.glob(os.path.join(root, "mt", "*.json"))):
        j = load(p)
        mt.setdefault(j["label"], {})[j["workload"]["name"]] = j
    rd_path = os.path.join(root, "realdata", "bench.json")
    rd = load(rd_path) if os.path.exists(rd_path) else None
    first = next(iter(single.values()), None) or (next(iter(next(iter(mt.values())).values())) if mt else rd)
    if first is None:
        sys.exit(f"no result files under {root}")
    env = first["environment"]
    clk = env["clock"]
    tick_ns = 1.0 / clk["ticks_per_ns"]

    out = []
    out.append(f"# Benchmark results: {env['cpu']}\n")
    out.append(f"- OS: {env['os']}; {env['logical_cores']} logical cores; commit `{first['commit']}`")
    out.append(f"- Flags: `{env['flags']}`")
    out.append(f"- Clock: {clk['name']}; one tick = {tick_ns:.2f} ns; timestamp-pair overhead {clk['timestamp_pair_overhead_ns']:.0f} ns (not subtracted)")
    if tick_ns > 5:
        out.append(f"- **Latency percentiles are quantised to {tick_ns:.1f} ns** on this machine (its timer is coarse). "
                   "Throughput comes from the wall clock over the whole loop and is not affected.")
    out.append(f"- Machine state: {env['note']} CPU load before runs: "
               + ", ".join(sorted({f'{x}%' for x in [first['environment']['cpu_load_percent_before_run']]})))
    out.append("")

    if single:
        out.append("## Single-thread engine\n")
        out.append("| workload | Mops/s (median) | CV % | p50 ns | p99 ns | p99.9 ns | allocs/op | pinned CPU |")
        out.append("|---|---|---|---|---|---|---|---|")
        for w, j in single.items():
            t, l = j["throughput_ops_per_sec"], j["latency_ns"]
            out.append(f"| {w} | {med(t) / 1e6:.2f} | {t['cv_pct']:.1f} | {med(l['p50']):.0f} | {med(l['p99']):.0f} | "
                       f"{med(l['p99_9']):.0f} | {j['allocations_per_op']:.3f} | {j['environment']['pinned_cpu']} |")
        out.append("")

    if mt:
        e = next(iter(next(iter(mt.values())).values()))["environment"]
        out.append("## Queue designs (producer thread -> queue -> matching thread)\n")
        out.append(f"- Pinning: {e['pinning']}")
        c = e["clock"]
        out.append(f"- Cross-thread timestamps: synchronised counter = {c['invariant_tsc_cpuid']}; ping-pong offset estimate "
                   f"{c['cross_core_offset_ns_estimate']:.0f} ns, minimum one-way hand-off {c['min_one_way_handoff_ns']:.0f} ns\n")
        out.append("| queue | workload | Mops/s | paced p50 ns | paced p99 ns | behind schedule | checksum = single-thread |")
        out.append("|---|---|---|---|---|---|---|")
        for label, byw in mt.items():
            for w in WORKLOADS:
                if w not in byw:
                    continue
                j = byw[w]
                out.append(f"| {label} | {w} | {med(j['throughput_ops_per_sec']) / 1e6:.2f} | {med(j['latency_ns']['p50']):.0f} | "
                           f"{med(j['latency_ns']['p99']):.0f} | {med(j['behind_schedule_fraction']):.3f} | {j['checksum_matches_single_thread']} |")
        out.append("")
        bad = [f"{lbl}/{w}" for lbl, byw in mt.items() for w, j in byw.items() if not j["checksum_matches_single_thread"]]
        if bad:
            out.append(f"**CHECKSUM MISMATCH in: {', '.join(bad)}**\n")

    if rd:
        t, l = rd["throughput_ops_per_sec"], rd["latency_ns"]
        out.append("## Real Nasdaq ITCH replay (one trading day, 10 symbols)\n")
        out.append(f"{rd['workload']['ops_replayed']:,} engine operations over {rd['workload']['books']} books: "
                   f"{med(t) / 1e6:.2f} Mops/s median (CV {t['cv_pct']:.1f}%), p50 {med(l['p50']):.0f} ns, "
                   f"p99 {med(l['p99']):.0f} ns, p99.9 {med(l['p99_9']):.0f} ns. Pinned CPU: {rd['environment']['pinned_cpu']}.\n")

    text = "\n".join(out)
    with open(os.path.join(root, "SUMMARY.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
