#!/usr/bin/env python3
"""Regenerate the visualizer's data files.

  viz/data/sample_run.js   synthetic market run (needs the lob_sim binary: --sim PATH)
  viz/data/latency.js      latency histograms + percentiles copied from bench/results/*.json
                           (Stage 3 book steps, Stage 4 queue variants). No number is typed by hand.

Usage:
  python scripts/make_viz_data.py --sim build-rel/tools/lob_sim.exe        # both
  python scripts/make_viz_data.py                                          # latency only
  python scripts/make_viz_data.py --sim ... --check                        # fail if files would change
"""
import argparse, glob, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "viz", "data")
RES = os.path.join(ROOT, "bench", "results")

# Parameters of the committed sample run. Change them here, regenerate, commit.
SAMPLE_ARGS = ["--seed", "7", "--ops", "8000", "--rate", "50000", "--cancel", "0.30", "--modify", "0.08", "--aggr", "0.15",
               "--depth-dist", "geometric", "--depth-mean", "6", "--mid", "10000", "--snapshot-every", "8", "--levels", "15"]


def pct(j, key):
    return j["latency_ns"][key]["median"]


def entry(study, variant, title, j, rel):
    env = j["environment"]
    ops = j["workload"]["ops"]
    reps = j["method"]["measured_reps"] if "method" in j else j["config"]["measured_reps"]
    return {
        "study": study, "workload": j["workload"]["name"], "variant": variant, "variant_title": title,
        "histogram_log2_ns": j["latency_histogram_log2_ns"]["counts"],
        "samples": ops * reps,
        "p50_ns": pct(j, "p50"), "p99_ns": pct(j, "p99"), "p99_9_ns": pct(j, "p99_9"), "max_ns": pct(j, "max"),
        "commit": j["commit"], "file": rel,
        "environment": {"cpu": env["cpu"].strip(), "os": env["os"], "cpu_load_percent_before_run": env["cpu_load_percent_before_run"]},
        "kind": "per-op service time (Stage 3, single thread)" if study == "stage3" else
                "end-to-end: intended send time to matching-thread completion (Stage 4, paced producer)",
    }


def build_latency():
    steps = json.load(open(os.path.join(RES, "steps.json")))
    out = []
    for f in sorted(glob.glob(os.path.join(RES, "*_*.json"))):
        j = json.load(open(f))
        lab = j["label"]
        if lab not in steps or steps[lab].get("status") == "re-measure":
            continue
        out.append(entry("stage3", lab, f"{lab}: {steps[lab]['title']}", j, os.path.relpath(f, ROOT).replace("\\", "/")))
    s4 = os.path.join(RES, "stage4")
    for f in sorted(glob.glob(os.path.join(s4, "*.json"))):
        j = json.load(open(f))
        out.append(entry("stage4", j["label"], j["label"], j, os.path.relpath(f, ROOT).replace("\\", "/")))
    return {"format": "lob-latency-v1", "sources": out}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    os.makedirs(DATA, exist_ok=True)
    outputs = {}
    outputs[os.path.join(DATA, "latency.js")] = "window.LOB_LATENCY = " + json.dumps(build_latency(), separators=(",", ":")) + ";\n"
    if a.sim:
        tmp = os.path.join(DATA, "sample_run.js.tmp")
        r = subprocess.run([a.sim] + SAMPLE_ARGS + ["--js", tmp], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr or r.stdout)
        print(r.stdout.strip())
        outputs[os.path.join(DATA, "sample_run.js")] = open(tmp, newline="").read().replace("\r\n", "\n")
        os.remove(tmp)
    changed = False
    for p, content in outputs.items():
        old = open(p, newline="").read().replace("\r\n", "\n") if os.path.exists(p) else None
        if old != content:
            changed = True
            if a.check:
                print("would change", os.path.relpath(p, ROOT))
            else:
                open(p, "w", newline="").write(content)
                print("wrote", os.path.relpath(p, ROOT))
    if a.check and changed:
        sys.exit(1)


if __name__ == "__main__":
    main()
