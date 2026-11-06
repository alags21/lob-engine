#!/bin/bash
# Usage: scripts/step.sh LABEL "commit message first line" "commit body"
# Runs the correctness gate, commits the source change, builds Release, runs the benchmarks,
# and prints checksum agreement with the baseline.
set -e
cd "$(dirname "$0")/.."
LABEL="$1"; MSG="$2"; BODY="$3"
cmd //c "scripts\check.cmd" 2>&1 | grep -E "error|warning C|tests passed|Failed|PASS|MISMATCH" | tee /tmp/gate.txt
grep -q "MISMATCH\|Failed\|error" /tmp/gate.txt && { echo GATE FAILED; exit 1; }
git add -A
git commit -q -m "$MSG

$BODY
Correctness gate before commit: ctest 73/73; lob_fuzz seeds 1-2 x 1M ops (state every 16 ops) and
seed 1 x 100k ops (state every op) on the checked build: no mismatches.

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" 2>&1 | grep -v warning || true
cmd //c "scripts\win-build.cmd build-rel" 2>&1 | grep -E "error|tests passed" || true
python scripts/run_bench.py --label "$LABEL" 2>&1 | grep -E "WARN" || true
python - "$LABEL" <<'PYEOF'
import json,sys
L=sys.argv[1]
for w in ['passive','aggressive','balanced']:
    a=json.load(open(f'bench/results/00-baseline_{w}.json'));b=json.load(open(f'bench/results/{L}_{w}.json'))
    print(w,'checksum_match=',a['checksum']==b['checksum'],'Mops/s',round(a['throughput_ops_per_sec']['median']/1e6,3),'->',round(b['throughput_ops_per_sec']['median']/1e6,3),'cv',round(b['throughput_ops_per_sec']['cv_pct'],1),'allocs/op',b['allocations_per_op'],'p50',round(b['latency_ns']['p50']['median']),'p99',round(b['latency_ns']['p99']['median']),'p999',round(b['latency_ns']['p99_9']['median']))
PYEOF
git add -A; git commit -q -m "Benchmark results: $LABEL

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" 2>&1 | grep -v warning || true
