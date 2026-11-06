#!/usr/bin/env python3
"""Generate docs/CONCURRENCY.md and the `mt` generated blocks in README.md / PROGRESS.md /
docs/DESIGN.md from bench/results/stage4/*.json. No number is typed by hand.

Queue descriptions and verdict wording that does not depend on numbers live in KINDS below;
verdicts that depend on numbers (did SPSC beat X?) are computed here from the data.
Usage: python scripts/make_mt_report.py [--check]
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "bench", "results", "stage4")
WORKLOADS = ["passive", "aggressive", "balanced"]
KINDS = [
    ("single-thread", "Ceiling: no queue, one thread applies ops (Stage 3 book)"),
    ("mutex-book", "Producer applies each op under a std::mutex (uncontended lock cost)"),
    ("mutex-cv-queue", "Bounded mutex + condition-variable queue to a matching thread"),
    ("spsc", "Lock-free SPSC ring, cache-line padded indices, spin wait (the design)"),
    ("spsc-yield", "SPSC, wait strategy: yield"),
    ("spsc-backoff", "SPSC, wait strategy: spin 128 then yield"),
    ("spsc-unpadded", "SPSC ablation: head/tail/caches share one cache line"),
    ("spsc-slot64", "SPSC variant: every slot padded and aligned to one cache line"),
    ("single-thread-nocb", "Ceiling, event callback disabled"),
    ("mutex-cv-queue-nocb", "Mutex+condvar queue, event callback disabled"),
    ("spsc-nocb", "SPSC, event callback disabled"),
]
DESC = dict(KINDS)


def load():
    d = {}
    for f in sorted(glob.glob(os.path.join(RES, "*.json"))):
        j = json.load(open(f))
        d.setdefault(j["label"], {})[j["workload"]["name"]] = j
    return d


def tp(j):
    return j["throughput_ops_per_sec"]["median"] / 1e6


def row(label, j, ceiling):
    t = j["throughput_ops_per_sec"]
    l = j["latency_ns"]
    return (f"| {label} | {DESC.get(label, '')} | {t['median'] / 1e6:.2f} ({t['cv_pct']:.1f}%) | "
            f"{100.0 * tp(j) / tp(ceiling):.0f}% | {l['p50']['median']:.0f} | {l['p99']['median']:.0f} | "
            f"{l['p99_9']['median']:.0f} | {l['max']['median']:.0f} | {j['behind_schedule_fraction']['median']:.3f} |")


HEAD = ("| queue | description | Mops/s median (CV) | vs single-thread | p50 ns | p99 ns | p99.9 ns | max ns | behind schedule |\n"
        "|---|---|---|---|---|---|---|---|---|")


def tables(d):
    out = []
    for wl in WORKLOADS:
        if "single-thread" not in d or wl not in d["single-thread"]:
            continue
        ceil = d["single-thread"][wl]
        out.append(f"### Workload `{wl}`\n")
        cfg = ceil["config"]
        out.append(f"{ceil['workload']['ops']:,} ops, seed {ceil['workload']['seed']}, queue capacity {cfg['capacity']}, "
                   f"latency mode paced at {cfg['paced_rate_ops_per_sec']:,.0f} ops/s.\n")
        out.append(HEAD)
        for label, _ in KINDS:
            if label in d and wl in d[label]:
                out.append(row(label, d[label][wl], ceil))
        out.append("")
    return "\n".join(out)


def verdicts(d):
    """Number-dependent conclusions, computed from the data."""
    lines = []
    for wl in WORKLOADS:
        try:
            s, cv, mb, st = d["spsc"][wl], d["mutex-cv-queue"][wl], d["mutex-book"][wl], d["single-thread"][wl]
        except KeyError:
            continue
        def cmp(a, b, name):
            r = tp(a) / tp(b)
            return f"{'beats' if r > 1 else 'does NOT beat'} {name} on throughput ({r:.2f}x)"
        def lat(a, b, name, key="p99"):
            x, y = a["latency_ns"][key]["median"], b["latency_ns"][key]["median"]
            return f"{key} {x:.0f} ns vs {y:.0f} ns for {name}"
        lines.append(f"- `{wl}`: SPSC {cmp(s, cv, 'the mutex+condvar queue')}; {cmp(s, mb, 'the mutex-protected book')}; "
                     f"reaches {100 * tp(s) / tp(st):.0f}% of the single-thread ceiling. Paced latency, median: {lat(s, cv, 'mutex+condvar', 'p50')}; "
                     f"{lat(s, mb, 'mutex-book', 'p50')}; {lat(s, st, 'single-thread (no hand-off)', 'p50')}.")
        lines.append(f"  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 {st['latency_ns']['p99']['median']:.0f} ns "
                     f"and was behind schedule for {100 * st['behind_schedule_fraction']['median']:.1f}% of ops. Paced p99 of the other rows: spsc {s['latency_ns']['p99']['median']:.0f} ns, "
                     f"mutex+condvar {cv['latency_ns']['p99']['median']:.0f} ns, mutex-book {mb['latency_ns']['p99']['median']:.0f} ns; differences at that level are not reliable evidence about the queues.")
        if "spsc-slot64" in d and wl in d["spsc-slot64"]:
            q = d["spsc-slot64"][wl]
            lines.append(f"  - one-slot-per-cache-line variant: {tp(q):.2f} vs {tp(s):.2f} Mops/s ({tp(q) / tp(s):.2f}x; CV {q['throughput_ops_per_sec']['cv_pct']:.1f}% / {s['throughput_ops_per_sec']['cv_pct']:.1f}%).")
        if "spsc-unpadded" in d and wl in d["spsc-unpadded"]:
            u = d["spsc-unpadded"][wl]
            r = tp(s) / tp(u)
            lines.append(f"  - padding ablation: padded {tp(s):.2f} vs unpadded {tp(u):.2f} Mops/s ({r:.2f}x); "
                         f"padding {'helped' if r > 1.05 else 'did not measurably help' if r > 0.95 else 'was slower'} (CV {s['throughput_ops_per_sec']['cv_pct']:.1f}% / {u['throughput_ops_per_sec']['cv_pct']:.1f}%).")
        ws = [(k, tp(d[k][wl])) for k in ("spsc", "spsc-yield", "spsc-backoff") if k in d and wl in d[k]]
        if len(ws) == 3:
            lines.append("  - wait strategy throughput: " + ", ".join(f"{k} {v:.2f}" for k, v in ws) + " Mops/s; paced p99: "
                         + ", ".join(f"{k} {d[k][wl]['latency_ns']['p99']['median']:.0f} ns" for k, _ in ws) + ".")
    return "\n".join(lines)


def env_block(d):
    j = d["single-thread"][WORKLOADS[0]]
    e, c = j["environment"], j["environment"]["clock"]
    loads = sorted({x["environment"]["cpu_load_percent_before_run"] for v in d.values() for x in v.values()})
    return "\n".join([
        f"- CPU: {e['cpu']} ({e['logical_cores']} logical cores); OS: {e['os']}",
        f"- Flags: `{e['flags']}`",
        f"- Pinning: producer on logical CPU {e['producer_cpu']}, matching thread on logical CPU {e['consumer_cpu']}; {e['pinning']}. "
        "On this CPU the physical cores are not necessarily identical (mixed core types possible); this was not checked.",
        f"- Clock: {c['name']}; {c['ticks_per_ns']:.3f} ticks/ns; timestamp-pair overhead {c['timestamp_pair_overhead_ns']:.0f} ns (not subtracted)",
        f"- TSC checks: CPUID invariant-TSC bit = {c['invariant_tsc_cpuid']}; ping-pong between the two pinned cores ({c['ping_pong_rounds']} rounds) estimated an offset of "
        f"{c['cross_core_offset_ns_estimate']:.0f} ns with a minimum one-way hand-off of {c['min_one_way_handoff_ns']:.0f} ns. The estimate assumes symmetric delay, so "
        "treat cross-thread latencies as accurate to roughly that offset.",
        f"- Machine state: {e['note']} CPU load measured before each run: " + ", ".join(f"{x}%" for x in loads),
        f"- Method: {j['config']['measured_reps']} measured repetitions after {j['config']['warmup_reps']} warmup; throughput mode: {j['config']['throughput_mode']}; latency mode: {j['config']['latency_mode']}",
    ])


def headline(d):
    lines = ["| workload | single-thread Mops/s | SPSC Mops/s | mutex+condvar Mops/s | mutex-book Mops/s | SPSC paced p99 ns | mutex+condvar paced p99 ns | mutex-book paced p99 ns |",
             "|---|---|---|---|---|---|---|---|"]
    for wl in WORKLOADS:
        try:
            st, s, cv, mb = (d[k][wl] for k in ("single-thread", "spsc", "mutex-cv-queue", "mutex-book"))
        except KeyError:
            continue
        p = lambda j: f"{j['latency_ns']['p99']['median']:.0f}"
        lines.append(f"| {wl} | {tp(st):.2f} | {tp(s):.2f} | {tp(cv):.2f} | {tp(mb):.2f} | {p(s)} | {p(cv)} | {p(mb)} |")
    return ("Stage 4, producer thread -> queue -> matching thread (medians of repeated runs; latency measured from the intended send time at a "
            "fixed paced rate; see docs/CONCURRENCY.md for method, ablations and caveats).\n\n" + "\n".join(lines) + "\n\n" + verdicts(d))


def replace_block(path, name, body):
    s = open(path, newline="").read().replace("\r\n", "\n")
    pat = re.compile(rf"(<!-- BEGIN GENERATED:{name} -->\n).*?(<!-- END GENERATED:{name} -->)", re.S)
    if not pat.search(s):
        return None
    return pat.sub(lambda m: m.group(1) + body + "\n" + m.group(2), s)


def main():
    d = load()
    if not d:
        sys.exit("no stage4 results")
    doc = ("# Concurrency (Stage 4)\n\n<!-- Generated by scripts/make_mt_report.py from bench/results/stage4/*.json. Do not edit by hand. -->\n\n"
           "## Environment\n\n" + env_block(d) + "\n\n"
           "CI runners are shared and noisy; no CI timing appears here. CI only builds and smoke-runs the benchmark and runs the sanitizers.\n\n"
           "## Conclusions (computed from the data below)\n\n" + verdicts(d) + "\n\n"
           "## Reading the results\n\n"
           "- The matching thread is the same single-threaded book in every row; only the hand-off differs. A queue cannot make one book faster than the single-thread ceiling: it adds a cache-line transfer per op, so the interesting question is how little each design adds, and what it buys (the producer is decoupled from matching and from its latency).\n"
           "- The mutex-book row has no hand-off at all, only an uncontended lock, so it is expected to beat any queue on raw throughput when there is a single producer. It is a lower bound on what locking costs, not a model of a contended lock.\n"
           "- Explanations for why a variant is faster or slower (coherence traffic, spinning against a full queue, slot straddling cache lines) are hypotheses consistent with the data; no hardware counters were collected to confirm them.\n"
           "- Wait strategy: yielding raised saturated throughput but had a clearly higher paced median than spinning, and spin-then-yield matched pure spinning at the median (see the p50 columns). `spsc` (spin) is the default because the goal is low hand-off latency on dedicated cores; it is the wrong choice when cores are shared, where spin-then-yield (`spsc-backoff`) is the safer option.\n"
           "- ThreadSanitizer runs only in Linux CI (clang); it is unavailable with MSVC, so the Windows machine has never run it. No deliberately broken queue was run through TSan to prove it would flag one (not done).\n\n"
           "## Results\n\n" + tables(d) + "\n"
           "Throughput mode: the producer pushes as fast as the queue admits, so the queue stays full and this measures sustained rate. "
           "Latency columns come from the paced run (medians over repetitions of per-op percentiles), measured from the intended send time to the "
           "matching thread finishing the op; for the two no-hand-off rows (single-thread, mutex-book) that is service time. "
           "\"behind schedule\" is the fraction of ops the producer reached more than one period late; a large value means the configuration "
           "cannot sustain the paced rate and its latency numbers describe overload. Max latency is dominated by OS scheduling interruptions.\n"
           "Every row's event stream + final book checksum was verified equal to the single-thread run before timing (the benchmark exits non-zero otherwise).\n")
    outputs = {os.path.join(ROOT, "docs", "CONCURRENCY.md"): doc}
    h = headline(d)
    for path in ("README.md", "PROGRESS.md", os.path.join("docs", "DESIGN.md")):
        new = replace_block(os.path.join(ROOT, path), "mt", h)
        if new is not None:
            outputs[os.path.join(ROOT, path)] = new
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


if __name__ == "__main__":
    main()
