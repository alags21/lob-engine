#!/usr/bin/env python3
"""Run lob_mt_bench for every queue kind and workload (interleaved: workload outermost) and write
bench/results/stage4/<label>_<workload>.json. Environment facts come from run_bench.py helpers.

  python scripts/run_mt_bench.py [--build build-rel] [--ops 1000000] [--rate 500000] [--reps 7]
"""
import argparse, os, platform, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_bench as rb  # noqa: E402

KINDS = ["single-thread", "mutex-book", "mutex-cv-queue", "spsc", "spsc-yield", "spsc-backoff", "spsc-unpadded", "spsc-slot64"]
NOCB = ["single-thread", "mutex-cv-queue", "spsc"]  # also measured with the event callback disabled


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build-rel")
    ap.add_argument("--ops", type=int, default=1_000_000)
    ap.add_argument("--rate", type=int, default=500_000)
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--capacity", type=int, default=4096)
    ap.add_argument("--workloads", nargs="+", default=["passive", "aggressive", "balanced"])
    ap.add_argument("--kinds", nargs="+", default=KINDS)
    ap.add_argument("--no-nocb", action="store_true")
    ap.add_argument("--outdir", default="bench/results/stage4")
    ap.add_argument("--note", default="Desktop machine, not isolated: normal background load, turbo/boost not controlled.")
    a = ap.parse_args()
    exe = os.path.join(rb.ROOT, a.build, "bench", "lob_mt_bench" + (".exe" if os.name == "nt" else ""))
    sha = rb.sh(["git", "rev-parse", "--short", "HEAD"])
    if rb.sh(["git", "status", "--porcelain", "--", "include", "src", "bench/mt_bench.cpp", "bench/workload.hpp"]):
        sha += "+dirty"
        print("WARNING: uncommitted changes in sources; results marked +dirty", file=sys.stderr)
    out = os.path.join(rb.ROOT, a.outdir)
    os.makedirs(out, exist_ok=True)
    runs = [(k, "on") for k in a.kinds] + ([] if a.no_nocb else [(k, "off") for k in NOCB if k in a.kinds])
    for wl in a.workloads:
        for kind, cb in runs:
            label = kind if cb == "on" else kind + "-nocb"
            load = rb.cpu_load()
            cmd = [exe, "--queue", kind, "--workload", wl, "--ops", str(a.ops), "--reps", str(a.reps), "--rate", str(a.rate),
                   "--capacity", str(a.capacity), "--callback", cb, "--label", label, "--commit", sha, "--os", platform.platform(),
                   "--flags", rb.cmake_flags(a.build), "--load", str(load), "--note", a.note,
                   "--out", os.path.join(out, f"{label}_{wl}.json")]
            print(label, wl, flush=True)
            subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
