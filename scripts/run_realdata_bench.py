#!/usr/bin/env python3
"""Runs the real-stream replay benchmark (lob_itch bench) with an environment record.

Waits and retries while the machine is busy (other workloads), pins to a performance-class core and
records the CPU load measured just before the run. The ITCH file stays outside the repository.

  python scripts/run_realdata_bench.py --exe PATH/lob_itch.exe --input PATH/07302019.NASDAQ_ITCH50.gz \
      --out bench/results/realdata/bench.json [--symbols A,B,...] [--cpu 4] [--max-load 30]
"""
import argparse, os, platform, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_bench as rb  # noqa: E402

DEFAULT_SYMBOLS = "SPY,QQQ,IWM,URTY,TQQQ,AAPL,MSFT,MU,TSLA,SCHP"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--symbols", default=DEFAULT_SYMBOLS)
    ap.add_argument("--cpu", type=int, default=4)
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--max-load", type=float, default=30.0)
    ap.add_argument("--retries", type=int, default=6)
    ap.add_argument("--build", default="build-rel")
    a = ap.parse_args()
    sha = rb.sh(["git", "rev-parse", "--short", "HEAD"])
    for attempt in range(a.retries + 1):
        load = rb.cpu_load()
        print(f"attempt {attempt}: CPU load before run {load}%", flush=True)
        if load <= a.max_load or attempt == a.retries:
            break
        print("machine busy, waiting 3 minutes", flush=True)
        time.sleep(180)
    note = (f"Desktop machine, not isolated; other workloads may run. CPU load measured before the run: {load}% "
            f"(limit for starting: {a.max_load}%, attempt {attempt + 1}).")
    cmd = [a.exe, "bench", "--input", a.input, "--json", a.out, "--symbols", a.symbols, "--reps", str(a.reps), "--cpu", str(a.cpu),
           "--label", "realdata-replay", "--commit", sha, "--os", platform.platform(), "--flags", rb.cmake_flags(a.build), "--load", str(load), "--note", note]
    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
