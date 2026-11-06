#!/usr/bin/env python3
"""Generate docs/PERFORMANCE.md and the generated blocks in README.md / PROGRESS.md / docs/DESIGN.md
from bench/results/*.json. No number in those blocks is typed by hand.

Step titles and kept/rejected status come from bench/results/steps.json (text only).
Usage: python scripts/make_report.py [--check]   (--check exits 1 if files would change)
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "bench", "results")
WORKLOADS = ["passive", "aggressive", "balanced"]


def load():
    data = {}
    for f in sorted(glob.glob(os.path.join(RES, "*_*.json"))):
        j = json.load(open(f))
        data.setdefault(j["label"], {})[j["workload"]["name"]] = j
    return data


def mops(j):
    return j["throughput_ops_per_sec"]["median"] / 1e6


def row(label, title, status, j, base):
    t = j["throughput_ops_per_sec"]
    l = j["latency_ns"]
    match = "yes" if j["checksum"] == base["checksum"] else "NO"
    return (f"| {label} | {title} | {status} | {mops(j):.2f} ({t['cv_pct']:.1f}%) | "
            f"{mops(j) / mops(base):.2f}x | {l['p50']['median']:.0f} | {l['p99']['median']:.0f} | "
            f"{l['p99_9']['median']:.0f} | {l['max']['median']:.0f} | {j['allocations_per_op']:.3f} | {match} |")


HEAD = ("| step | change | status | Mops/s median (CV) | vs baseline | p50 ns | p99 ns | p99.9 ns | max ns | "
        "allocs/op | same events as baseline |\n|---|---|---|---|---|---|---|---|---|---|---|")


def tables(data, steps):
    out = []
    labels = sorted(data)
    base_label = labels[0]
    for wl in WORKLOADS:
        if wl not in data[base_label]:
            continue
        base = data[base_label][wl]
        w = base["workload"]
        out.append(f"### Workload `{wl}`\n")
        out.append(f"{w['submit_pct']}% submit / {w['cancel_pct']}% cancel / {w['modify_pct']}% modify targets, "
                   f"{w['aggressive_pct']}% of submits aggressive, book target {w['book_target']} orders, "
                   f"{w['ops']:,} ops, seed {w['seed']}. Realised: {w['realised']['submits']:,} submits, "
                   f"{w['realised']['cancels']:,} cancels, {w['realised']['modifies']:,} modifies; "
                   f"{w['trades']:,} trades; {w['final_resting_orders']:,} orders resting at the end.\n")
        out.append(HEAD)
        for lab in labels:
            if wl not in data[lab]:
                continue
            s = steps.get(lab, {})
            out.append(row(lab, s.get("title", ""), s.get("status", ""), data[lab][wl], base))
        out.append("")
    return "\n".join(out)


def env_block(data):
    j = data[sorted(data)[0]][WORKLOADS[0]]
    e = j["environment"]
    loads = sorted({x["environment"]["cpu_load_percent_before_run"] for d in data.values() for x in d.values()})
    shas = sorted({f"{lab}: {d[WORKLOADS[0]]['commit']}" for lab, d in data.items()})
    return "\n".join([
        f"- CPU: {e['cpu']} ({e['logical_cores']} logical cores)",
        f"- OS: {e['os']}",
        f"- Compiler: {e['compiler']}",
        f"- Flags: `{e['flags']}`",
        f"- Thread pinned to logical CPU {e['pinned_cpu']}; process/thread priority raised on Windows",
        f"- Clock: {e['clock']['name']}; {e['clock']['ticks_per_ns']:.3f} ticks/ns; timestamp-pair overhead "
        f"{e['clock']['timestamp_pair_overhead_ns']:.0f} ns (not subtracted from latencies)",
        f"- Machine state: {e['note']} CPU load measured just before each run ranged over: "
        + ", ".join(f"{x}%" for x in loads),
        f"- Method: {j['method']['model']}; {j['method']['warmup_reps']} untimed warmup repetition, "
        f"{j['method']['measured_reps']} measured repetitions on a fresh book each; {j['method']['throughput']}; "
        f"{j['method']['latency']}",
        "- Commits benchmarked: " + "; ".join(shas),
    ])


def headline(data, steps):
    labels = sorted(data)
    base_label, final = labels[0], labels[-1]
    # final = last label that is not a re-measurement of the baseline
    for lab in reversed(labels):
        if steps.get(lab, {}).get("status") != "re-measure":
            final = lab
            break
    lines = ["| workload | baseline Mops/s | final Mops/s | speedup | baseline p99 ns | final p99 ns | baseline p99.9 ns | final p99.9 ns | allocs/op before -> after |",
             "|---|---|---|---|---|---|---|---|---|"]
    for wl in WORKLOADS:
        b, f = data[base_label].get(wl), data[final].get(wl)
        if not b or not f:
            continue
        lines.append(f"| {wl} | {mops(b):.2f} | {mops(f):.2f} | {mops(f) / mops(b):.2f}x | "
                     f"{b['latency_ns']['p99']['median']:.0f} | {f['latency_ns']['p99']['median']:.0f} | "
                     f"{b['latency_ns']['p99_9']['median']:.0f} | {f['latency_ns']['p99_9']['median']:.0f} | "
                     f"{b['allocations_per_op']:.3f} -> {f['allocations_per_op']:.3f} |")
    return f"Baseline `{base_label}` vs `{final}` (medians of repeated runs; see docs/PERFORMANCE.md for method, variance and every step).\n\n" + "\n".join(lines)


def pcore_section(data):
    """Supplementary check: baseline vs final re-run pinned to a higher-performance core (see bench/results/pcore)."""
    files = sorted(glob.glob(os.path.join(RES, "pcore", "*.json")))
    if not files:
        return ""
    pc = {}
    for f in files:
        j = json.load(open(f))
        pc.setdefault(j["label"], {})[j["workload"]["name"]] = j
    labs = sorted(pc)
    base, fin = labs[0], labs[-1]
    topo = os.path.join(RES, "cpu_topology.txt")
    cls = {}
    if os.path.exists(topo):
        for line in open(topo):
            m = re.match(r"logical\s+(\d+) core\s+(\d+) effclass (\d+)", line)
            if m:
                cls[int(m.group(1))] = int(m.group(3))
    main_cpu = data[sorted(data)[0]][WORKLOADS[0]]["environment"]["pinned_cpu"]
    pcpu = pc[base][WORKLOADS[0]]["environment"]["pinned_cpu"]
    out = ["", "## Core-type check", "",
           f"This CPU has mixed core types (see bench/results/cpu_topology.txt). The main results above were pinned to logical CPU {main_cpu}, "
           f"which Windows reports as efficiency class {cls.get(main_cpu, '?')}; the highest class is {max(cls.values()) if cls else '?'}. "
           f"As a check, the baseline and final binaries were re-run pinned to logical CPU {pcpu} (efficiency class {cls.get(pcpu, '?')}), "
           "same workloads and method, each run once after the other (not interleaved with the main table):", "",
           "| workload | baseline Mops/s (CV) | final Mops/s (CV) | speedup | baseline p99 ns | final p99 ns |", "|---|---|---|---|---|---|"]
    for wl in WORKLOADS:
        if wl in pc[base] and wl in pc[fin]:
            b, f = pc[base][wl], pc[fin][wl]
            out.append(f"| {wl} | {mops(b):.2f} ({b['throughput_ops_per_sec']['cv_pct']:.1f}%) | {mops(f):.2f} ({f['throughput_ops_per_sec']['cv_pct']:.1f}%) | "
                       f"{mops(f) / mops(b):.2f}x | {b['latency_ns']['p99']['median']:.0f} | {f['latency_ns']['p99']['median']:.0f} |")
    return "\n".join(out) + "\n"


def replace_text(s, name, body):
    """Replaces the generated block `name` in text `s`; text without that block is returned unchanged."""
    pat = re.compile(rf"(<!-- BEGIN GENERATED:{name} -->\n).*?(<!-- END GENERATED:{name} -->)", re.S)
    return pat.sub(lambda m: m.group(1) + body + "\n" + m.group(2), s)


def steps_table(data, steps):
    """Speedup of every optimisation step over the baseline, per workload (medians)."""
    labels = [l for l in sorted(data) if steps.get(l, {}).get("status") != "re-measure"]
    base = labels[0]
    lines = ["| step | change | " + " | ".join(WORKLOADS) + " |", "|---|---|" + "---|" * len(WORKLOADS)]
    for lab in labels[1:]:
        cells = []
        for wl in WORKLOADS:
            cells.append(f"{mops(data[lab][wl]) / mops(data[base][wl]):.2f}x" if wl in data[lab] and wl in data[base] else "-")
        lines.append(f"| {lab} | {steps.get(lab, {}).get('title', '')}; {steps.get(lab, {}).get('status', '')} | " + " | ".join(cells) + " |")
    rer = [l for l in sorted(data) if steps.get(l, {}).get("status") == "re-measure"]
    if rer:
        lab = rer[-1]
        lines.append(f"| {lab} | unchanged baseline binary measured again (drift check) | " +
                     " | ".join(f"{mops(data[lab][wl]) / mops(data[base][wl]):.2f}x" for wl in WORKLOADS if wl in data[lab]) + " |")
    return "Speedup over the Stage 1 baseline (ratio of median throughput; the last row is the drift check, so differences smaller than its distance from 1.00x are not meaningful).\n\n" + "\n".join(lines)


def fuzz_block():
    """Differential-fuzz results copied from saved logs (docs/results/fuzz_*.txt). Counts only."""
    out = ["Differential fuzz against the reference model (all with zero mismatches; every line is a PASS line saved in `docs/results/`):", ""]
    for fname, title in (("fuzz_local_stage2.txt", "local, MSVC checked build"), ("fuzz_ci_stage2.txt", "CI, Stage 2 PR run")):
        path = os.path.join(ROOT, "docs", "results", fname)
        if not os.path.exists(path):
            continue
        for line in open(path):
            m = re.search(r"(?:^(.*?) \| .*? \| )?PASS runs=(\d+) first_seed=(\d+) ops=(\d+) events=(\d+) trades=(\d+) max_resting=(\d+)", line)
            if m:
                who = (m.group(1) + ", ") if m.group(1) else ""
                out.append(f"- {title}: {who}{m.group(2)} run(s) from seed {m.group(3)}, {int(m.group(4)):,} ops, {int(m.group(5)):,} events, "
                           f"{int(m.group(6)):,} trades, largest book {int(m.group(7)):,} orders")
    return "\n".join(out)


def main():
    data = load()
    if not data:
        sys.exit("no results")
    steps_path = os.path.join(RES, "steps.json")
    steps = json.load(open(steps_path)) if os.path.exists(steps_path) else {}
    bad = [(lab, wl) for lab, d in data.items() for wl, j in d.items()
           if j["checksum"] != data[sorted(data)[0]][wl]["checksum"]]
    perf = ("# Performance\n\n<!-- Generated by scripts/make_report.py from bench/results/*.json. Do not edit by hand. -->\n\n"
            "## Environment\n\n" + env_block(data) + "\n\n"
            "CI runners are shared and noisy; no CI timing is published here. CI only checks that the benchmark builds and runs.\n\n"
            "## Results\n\n" + tables(data, steps) + "\n"
            "Latency columns are medians across repetitions of per-op percentiles (overall, all op kinds). "
            "CV is the coefficient of variation of throughput across repetitions. "
            "The max column is dominated by OS scheduling interruptions on a non-isolated desktop, not by the book. The re-measure row runs the unchanged baseline binary again after the other steps of the same workload; its distance from the first baseline row is a direct estimate of drift between runs, so differences between steps smaller than that are not meaningful. "
            "\"Same events as baseline\" compares an FNV hash of every emitted event plus the final book between implementations on the identical op stream.\n")
    if bad:
        perf += f"\n**WARNING: checksum mismatch vs baseline: {bad}**\n"
    perf += pcore_section(data)
    outputs = {os.path.join(ROOT, "docs", "PERFORMANCE.md"): perf}
    blocks = {"perf": headline(data, steps), "perf-steps": steps_table(data, steps), "fuzz": fuzz_block()}
    for path in ("README.md", "PROGRESS.md", os.path.join("docs", "DESIGN.md")):
        full = os.path.join(ROOT, path)
        text = open(full, newline="").read().replace("\r\n", "\n")
        for name, body in blocks.items():
            text = replace_text(text, name, body)
        outputs[full] = text
    changed = False
    for p, content in outputs.items():
        old = open(p, newline="").read().replace("\r\n", "\n") if os.path.exists(p) else None
        if old != content:
            changed = True
            if "--check" not in sys.argv:
                open(p, "w", newline="").write(content)
                print("wrote", os.path.relpath(p, ROOT))
    if "--check" in sys.argv and changed:
        sys.exit(1)
    if bad:
        sys.exit("checksum mismatch: %s" % bad)


if __name__ == "__main__":
    main()
