#!/usr/bin/env python3
"""Generate the `platforms` block in README.md and docs/PLATFORMS.md from
bench/results/platforms/ci-*/ (one run of .github/workflows/bench-platforms.yml). No number is typed by hand.

Usage: python scripts/make_platform_report.py [--check]
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAT = os.path.join(ROOT, "bench", "results", "platforms")
WORKLOADS = ["passive", "aggressive", "balanced"]
ORDER = ["linux-x86_64", "linux-arm64", "macos-arm64", "macos-x86_64", "windows-x86_64"]
TARGETS = ["README.md", os.path.join("docs", "PLATFORMS.md")]


def load(p):
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def tput(d, kind, wl):
    return load(os.path.join(d, "mt", f"{kind}_{wl}.json"))["throughput_ops_per_sec"]["median"] / 1e6


def rng(vals, fmt="{:.2f}x"):
    lo, hi = min(vals), max(vals)
    return fmt.format(lo) if fmt.format(lo) == fmt.format(hi) else f"{fmt.format(lo)} to {fmt.format(hi)}"


def block():
    rows = []
    checks = 0
    for pid in ORDER:
        d = os.path.join(PLAT, f"ci-{pid}")
        if not os.path.isdir(d):
            continue
        first = load(os.path.join(d, "mt", f"spsc_{WORKLOADS[0]}.json"))
        cpu = " ".join(first["environment"]["cpu"].split())
        if cpu == "unknown":  # arm64 Linux: read the model from the recorded machine.txt
            mt = os.path.join(d, "machine.txt")
            m = re.search(r"Model name:\s*(.+)", open(mt, encoding="utf-8").read()) if os.path.exists(mt) else None
            cpu = m.group(1).strip() if m else cpu
        single = [tput(d, "single-thread", w) for w in WORKLOADS]
        spsc = [tput(d, "spsc", w) for w in WORKLOADS]
        cv = [tput(d, "mutex-cv-queue", w) for w in WORKLOADS]
        mb = [tput(d, "mutex-book", w) for w in WORKLOADS]
        for p in glob.glob(os.path.join(d, "mt", "*.json")):
            checks += 1
            assert load(p)["checksum_matches_single_thread"] is True, p
        rows.append(
            f"| {pid} | {cpu} | {'/'.join(f'{x:.1f}' for x in single)} | "
            f"{rng([a / b for a, b in zip(spsc, cv)])} | {rng([a / b for a, b in zip(spsc, mb)])} | "
            f"{rng([a / b for a, b in zip(spsc, single)])} |"
        )
    head = [
        "Same code and harness on GitHub-hosted shared VMs (`.github/workflows/bench-platforms.yml`, one run, medians of 7 repetitions per "
        "workload; the ratios compare designs measured in the same job). Absolute ops/s and tail latency are not reliable on shared VMs "
        "and macOS threads are not pinned; see [docs/PLATFORMS.md](docs/PLATFORMS.md).",
        "",
        "| platform | CPU | single-thread Mops/s (passive/aggressive/balanced) | SPSC vs mutex+condvar queue | SPSC vs mutex-protected book | SPSC vs single-thread ceiling |",
        "|---|---|---|---|---|---|",
    ]
    tail = ["", f"Every one of the {checks} queue runs above matched the single-thread event checksum. Ratios are a range over the three workloads."]
    return "\n".join(head + rows + tail)


def main():
    check = "--check" in sys.argv
    new = block()
    pat = re.compile(r"(<!-- BEGIN GENERATED:platforms -->\n).*?(<!-- END GENERATED:platforms -->)", re.S)
    bad = False
    for t in TARGETS:
        p = os.path.join(ROOT, t)
        old = open(p, encoding="utf-8").read()
        if not pat.search(old):
            sys.exit(f"{t}: missing GENERATED:platforms markers")
        upd = pat.sub(lambda m: m.group(1) + new + "\n" + m.group(2), old)
        if upd != old:
            if check:
                print(f"{t} is out of date; run scripts/make_platform_report.py")
                bad = True
            else:
                open(p, "w", encoding="utf-8", newline="\n").write(upd)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
