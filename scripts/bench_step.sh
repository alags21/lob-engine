#!/bin/bash
# Usage: scripts/bench_step.sh LABEL  -- build Release, run benchmarks, summarise vs baseline, commit results
cd "$(dirname "$0")/.."
LABEL="$1"
cmd //c "scripts\win-build.cmd build-rel" 2>&1 | grep -E " error |tests passed"
python scripts/run_bench.py --label "$LABEL" 2>&1 | grep -E "WARN"
python - "$LABEL" <<'PYEOF'
import json,sys
L=sys.argv[1]
for w in ['passive','aggressive','balanced']:
    a=json.load(open(f'bench/results/00-baseline_{w}.json'));b=json.load(open(f'bench/results/{L}_{w}.json'))
    print(w,'checksum_match=',a['checksum']==b['checksum'],'Mops/s',round(b['throughput_ops_per_sec']['median']/1e6,3),'cv',round(b['throughput_ops_per_sec']['cv_pct'],1),'allocs/op',b['allocations_per_op'],'p50',round(b['latency_ns']['p50']['median']),'p99',round(b['latency_ns']['p99']['median']),'p999',round(b['latency_ns']['p99_9']['median']))
PYEOF
git add -A; git commit -q -m "Benchmark results: $LABEL

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" 2>&1 | grep -v warning
exit 0
