#!/usr/bin/env python3
"""Re-measure every optimisation step from a clean checkout of its commit, interleaved.

For each workload, benchmarks every step's binary back to back (then the baseline again as
"99-baseline-rerun" to expose machine drift), so slow drift (thermal, boost, background load)
hits all steps alike. Each step is built in its own git worktree under --scratch.
Usage: python scripts/remeasure.py --scratch C:/tmp/lobwt   (steps listed in STEPS)
"""
import argparse, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STEPS = json_steps = None


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--steps", required=True, help="label=commit,label=commit,...")
    ap.add_argument("--rerun", default="99-baseline-rerun=00-baseline", help="label=existing-label to re-measure last")
    ap.add_argument("--cpu", type=int, default=8)
    ap.add_argument("--build-only", action="store_true")
    a = ap.parse_args()
    steps = [x.split("=") for x in a.steps.split(",")]
    trees = {}
    os.makedirs(a.scratch, exist_ok=True)
    for label, commit in steps:
        tree = os.path.join(a.scratch, label)
        trees[label] = tree
        if not os.path.exists(tree):
            run(["git", "worktree", "add", "--detach", tree, commit], cwd=ROOT)
        if not os.path.exists(os.path.join(tree, "build-rel", "bench", "lob_bench.exe")):
            run(["cmd", "/c", "scripts\win-build.cmd", "build-rel"], cwd=tree)
    if a.build_only:
        return
    rr_label, rr_of = a.rerun.split("=")
    for wl in ["passive", "aggressive", "balanced"]:
        for label, _ in steps:
            run([sys.executable, os.path.join(ROOT, "scripts", "run_bench.py"), "--label", label, "--tree", trees[label],
                 "--workloads", wl, "--cpu", str(a.cpu)])
        run([sys.executable, os.path.join(ROOT, "scripts", "run_bench.py"), "--label", rr_label, "--tree", trees[rr_of],
             "--workloads", wl, "--cpu", str(a.cpu)])


if __name__ == "__main__":
    main()
