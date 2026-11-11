#!/bin/bash
# One-command benchmark run for macOS on Apple Silicon (written for a fanless MacBook Air M3).
#
#   scripts/bench_macos.sh --quick                       # ~5 minutes: checks everything works
#   scripts/bench_macos.sh                               # full run (about 30-60 minutes)
#   scripts/bench_macos.sh --itch ~/Data/07302019.NASDAQ_ITCH50.gz    # also replay real Nasdaq data
#
# What it does: checks the machine state, installs missing build tools with Homebrew, builds
# Release, runs the tests and the differential-fuzz correctness gate, runs the single-thread,
# queue and (optionally) real-data benchmarks, and writes
#   bench/results/platforms/macos-<chip>/   (JSON per run, machine.txt, SUMMARY.md)
# plus a .tar.gz of that folder. Everything the script prints is also saved to run.log there.
#
# Options:
#   --quick          3 repetitions, 500k ops (smoke test; do NOT publish these numbers)
#   --itch FILE      path to the Nasdaq ITCH 5.0 .gz file (not in the repo; skipped if omitted)
#   --allow-battery  run on battery (not recommended: the CPU is throttled)
#   --id NAME        results folder name (default: macos-<chip>)
#   --no-install     never call brew; fail if a tool is missing
set -euo pipefail

QUICK=0; ITCH=""; ALLOW_BATTERY=0; ID=""; INSTALL=1
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --itch) ITCH="$2"; shift ;;
    --allow-battery) ALLOW_BATTERY=1 ;;
    --id) ID="$2"; shift ;;
    --no-install) INSTALL=0 ;;
    -h|--help) sed -n 2,24p "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")/.."
ROOT="$PWD"

# Keep the Mac awake for the whole run (re-run this script under caffeinate once).
if [ -z "${LOB_CAFFEINATED:-}" ]; then
  export LOB_CAFFEINATED=1
  exec caffeinate -dimsu "$SELF" "$@"
fi

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "this script is for macOS (use scripts/bench_windows.ps1 on Windows)"
[ "$(uname -m)" = "arm64" ] || die "expected Apple Silicon (arm64), found $(uname -m); if this shell runs under Rosetta, open a native Terminal"

CHIP="$(sysctl -n machdep.cpu.brand_string)"
[ -n "$ID" ] || ID="macos-$(echo "$CHIP" | tr 'A-Z ' 'a-z-')"
OUT="bench/results/platforms/$ID"
mkdir -p "$OUT/single" "$OUT/mt" "$OUT/realdata"
exec > >(tee "$OUT/run.log") 2>&1

echo "== $CHIP, $(sw_vers -productName) $(sw_vers -productVersion), results in $OUT"

# ---- machine state --------------------------------------------------------------------------
if ! pmset -g batt | head -1 | grep -q "AC Power"; then
  [ "$ALLOW_BATTERY" = 1 ] || die "plug in the charger (on battery macOS throttles the CPU), or pass --allow-battery"
  echo "WARNING: running on battery"
fi
if pmset -g | grep -Eiq "lowpowermode[[:space:]]+1"; then
  die "Low Power Mode is on: turn it off in System Settings > Battery"
fi
echo "Thermal state before run:"; pmset -g therm || true
echo
echo "For clean numbers: quit browsers, Slack, Xcode and anything else heavy; leave the lid open; do not touch the Mac while it runs."
echo "A fanless MacBook Air heats up under sustained load; the script pauses between phases and records thermal state."
echo

# ---- toolchain ------------------------------------------------------------------------------
xcode-select -p >/dev/null 2>&1 || die "Xcode command line tools missing: run 'xcode-select --install', then re-run"
need=""
for t in cmake ninja; do command -v "$t" >/dev/null 2>&1 || need="$need $t"; done
brew_has() { command -v brew >/dev/null 2>&1 && brew list --formula "$1" >/dev/null 2>&1; }
brew_has googletest || need="$need googletest"
if [ -n "$need" ]; then
  [ "$INSTALL" = 1 ] || die "missing:$need (install with: brew install$need)"
  command -v brew >/dev/null 2>&1 || die "Homebrew is required (https://brew.sh); then run: brew install$need"
  echo "Installing with Homebrew:$need"
  brew install $need
fi
command -v python3 >/dev/null 2>&1 || die "python3 not found (it comes with the Xcode command line tools)"
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"

# ---- build and correctness gate ---------------------------------------------------------------
BUILD=build-rel
echo "== configure and build (Release)"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$BREW_PREFIX"
cmake --build "$BUILD"
echo "== tests"
ctest --test-dir "$BUILD" --output-on-failure
echo "== differential fuzz vs the reference model"
FUZZ_OPS=1000000; [ "$QUICK" = 1 ] && FUZZ_OPS=200000
"$BUILD/tests/lob_fuzz" --seed 1 --ops "$FUZZ_OPS" --runs 2

# ---- machine record ---------------------------------------------------------------------------
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git: $(git rev-parse --short HEAD) $(git status --porcelain | head -1 | sed 's/.*/(dirty working tree)/')"
  echo "chip: $CHIP"; sysctl -n hw.perflevel0.logicalcpu hw.perflevel1.logicalcpu 2>/dev/null | paste -sd' ' | sed 's/^/performance+efficiency cores: /'
  echo "memory: $(( $(sysctl -n hw.memsize) / 1073741824 )) GB; cache line: $(sysctl -n hw.cachelinesize) B"
  echo "macOS: $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
  echo "compiler: $(c++ --version | head -1)"; echo "cmake: $(cmake --version | head -1)"
  echo "power: $(pmset -g batt | head -1)"; echo "thermal before:"; pmset -g therm
} > "$OUT/machine.txt"

# ---- benchmarks ---------------------------------------------------------------------------------
if [ "$QUICK" = 1 ]; then OPS=500000; REPS=3; MT_OPS=200000; else OPS=2000000; REPS=7; MT_OPS=1000000; fi
NOTE="$CHIP, macOS $(sw_vers -productVersion), fanless laptop on AC power, Low Power Mode off, not isolated: normal background load; threads are NOT pinned (macOS has no affinity API)."
[ "$QUICK" = 1 ] && NOTE="SMOKE TEST ONLY. $NOTE"
cool() { echo "-- cooling down 45 s (fanless machine)"; sleep 45; echo "thermal: $(pmset -g therm | tr '\n' ' ')"; }

echo "== single-thread benchmark"
python3 scripts/run_bench.py --label final --build "$BUILD" --cpu -1 --ops "$OPS" --reps "$REPS" --outdir "$OUT/single" --note "$NOTE"
cool
echo "== queue benchmark (producer -> queue -> matching thread)"
python3 scripts/run_mt_bench.py --build "$BUILD" --ops "$MT_OPS" --reps "$REPS" --outdir "$OUT/mt" --note "$NOTE"

if [ -n "$ITCH" ]; then
  [ -f "$ITCH" ] || die "ITCH file not found: $ITCH"
  cool
  echo "== real Nasdaq ITCH replay"
  python3 scripts/run_realdata_bench.py --exe "$BUILD/itch/lob_itch" --input "$ITCH" --out "$OUT/realdata/bench.json" \
    --cpu -1 --reps "$REPS" --build "$BUILD" --note-extra "$NOTE"
else
  echo "== real-data replay skipped (pass --itch FILE to include it)"
  rmdir "$OUT/realdata" 2>/dev/null || true
fi

{ echo "thermal after:"; pmset -g therm; } >> "$OUT/machine.txt"
python3 scripts/summarize_platform.py "$OUT"
tar -czf "$OUT.tar.gz" -C "$(dirname "$OUT")" "$ID"

echo
echo "== done. Results: $OUT (summary: $OUT/SUMMARY.md)"
echo "Send back the archive $ROOT/$OUT.tar.gz, or commit the folder:"
echo "    git checkout -b bench-$ID && git add $OUT && git commit -m 'Add $ID benchmark results' && git push -u origin HEAD"
[ "$QUICK" = 1 ] && echo "That was a --quick smoke test. Run again without --quick for publishable numbers."
exit 0
