#!/usr/bin/env python3
"""Run lob_bench for each workload and write bench/results/<label>_<workload>.json.

Records environment facts the C++ binary cannot know: git SHA, OS, compile flags (from the CMake
cache), and the CPU load measured just before the run. Usage:
  python scripts/run_bench.py --label 00-baseline [--build build-rel] [--cpu 8] [--ops N] [--reps R]
"""
import argparse, os, platform, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT).stdout.strip()


def cpu_load():
    """Average Win32_Processor.LoadPercentage over ~5 s, or -1 if unavailable."""
    if os.name != "nt":
        try:
            return round(os.getloadavg()[0] / (os.cpu_count() or 1) * 100, 1)
        except OSError:
            return -1
    ps = "$s=@(); 1..5 | % { $s += (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average; Start-Sleep 1 }; ($s | Measure-Object -Average).Average"
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout.strip()
    try:
        return round(float(out), 1)
    except ValueError:
        return -1


def cmake_flags(build):
    cache = os.path.join(ROOT, build, "CMakeCache.txt")
    vals = {}
    for line in open(cache, errors="replace"):
        m = re.match(r"(CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE|CMAKE_BUILD_TYPE):\w+=(.*)", line.strip())
        if m:
            vals[m.group(1)] = m.group(2)
    return f"{vals.get('CMAKE_BUILD_TYPE','')}: {vals.get('CMAKE_CXX_FLAGS','')} {vals.get('CMAKE_CXX_FLAGS_RELEASE','')}".strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--build", default="build-rel")
    ap.add_argument("--cpu", type=int, default=8)
    ap.add_argument("--ops", type=int, default=2_000_000)
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--workloads", nargs="+", default=["passive", "aggressive", "balanced"])
    ap.add_argument("--note", default="Desktop machine, not isolated: normal background load, turbo/boost not controlled.")
    ap.add_argument("--outdir", default="bench/results")
    a = ap.parse_args()

    exe = os.path.join(ROOT, a.build, "bench", "lob_bench" + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(exe):
        sys.exit(f"missing {exe}")
    sha = sh(["git", "rev-parse", "--short", "HEAD"])
    dirty = sh(["git", "status", "--porcelain", "--", "include", "src", "bench/bench_main.cpp", "bench/workload.hpp"])
    if dirty:
        sha += "+dirty"
        print("WARNING: uncommitted changes in sources; result marked +dirty", file=sys.stderr)
    os.makedirs(os.path.join(ROOT, a.outdir), exist_ok=True)
    env = os.environ.copy()
    if os.name == "nt":  # conda DLL dir not needed for the bench, but keep PATH as is
        pass
    for wl in a.workloads:
        load = cpu_load()
        out = os.path.join(ROOT, a.outdir, f"{a.label}_{wl}.json")
        cmd = [exe, "--workload", wl, "--ops", str(a.ops), "--reps", str(a.reps), "--cpu", str(a.cpu),
               "--label", a.label, "--commit", sha, "--os", platform.platform(), "--flags", cmake_flags(a.build),
               "--load", str(load), "--note", a.note, "--out", out]
        print(" ".join(cmd[:8]), "...", flush=True)
        subprocess.run(cmd, check=True, env=env)


if __name__ == "__main__":
    main()
