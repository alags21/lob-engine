# Progress

| Stage | Scope | Status | PR |
|---|---|---|---|
| 0 | Scaffold, CI | in review (CI green) | #1 |
| 1 | Core book | in review (CI green) | #2 |
| 2 | Correctness harness | in review (CI green) | #3 |
| 3 | Performance | in review | PR pending |
| 4 | Concurrency | not started | |
| 5 | Replay and visualizer | not started | |
| 6 | Polish | not started | |

## Built
Stage 0: CMake project (`lob` library, tests, bench, tools targets), clang-format, GitHub Actions
(Linux gcc/clang x Release/Debug, ASan+UBSan job), README, DESIGN skeleton, `docs/BENCHMARKING.md`
with the local Windows toolchain. Verified locally with MSVC 14.50 via `scripts/win-build.cmd`
(1 smoke test passes).

Stage 1: `lob::OrderBook` (`include/lob/`, `src/order_book.cpp`): limit, market, IOC, FOK, cancel,
modify; price-time priority; trades at maker price; synchronous event callback (Accepted, Rejected,
Trade, Cancelled, Modified); integer ticks and lots. 63 GoogleTest cases in
`tests/test_order_book.cpp` plus 1 smoke test (64 total, all passing locally with MSVC 14.50).

Stage 2: reference model (`tests/reference/`), seeded differential fuzzer (`tests/fuzz/`, `lob_fuzz`
executable plus a 5-seed x 40k-op GoogleTest), `OrderBook::validate()` invariant checker run after
every public call in non-NDEBUG builds, deterministic text op log / event format
(`docs/EVENT_FORMAT.md`, `lob_replay` tool, hand-checked golden log). 73 ctest cases total, all
passing locally. CI: fuzz in the Release jobs (1.5M ops per compiler: seeds 1-2 at 500k each, plus
one run-number-derived seed at 500k), Debug jobs (100k ops, state and invariants checked every
op), ASan+UBSan job (200k ops), and a scheduled nightly workflow (`nightly-fuzz.yml`, default
10 seeds x 5M ops, date-derived seeds).

## Measured results
Local fuzz run, `docs/results/fuzz_local_stage2.txt` (MSVC 14.50, checked build, state checked
every 16 ops): seeds 1, 2, 3 x 1,000,000 ops = 3,000,000 ops, 4,601,135 events emitted, 1,301,514
trades, 0 mismatches, largest resting book 4,928 orders, 178.9 s wall (includes the O(n) reference
model; this is not a throughput benchmark). Fuzzing found no bug in the Stage 1 book.

CI fuzz results (Actions run 42132275051 on PR #3, taken from the job logs, all passed with 0
mismatches): gcc and clang Release each ran seeds 1, 2 (500,000 ops each, 1,530,900 events,
430,010 trades) and seed 100005 (500,000 ops, 764,848 events, 214,644 trades), i.e. 1.5M ops per
compiler, in about 1.1-3.5 s per command. Debug (gcc, clang): seed 1, 100,000 ops, state and
invariants checked every op, 15-21 s. ASan+UBSan: seed 1, 200,000 ops, 216 s.

## Stage 3 headline (generated)

<!-- BEGIN GENERATED:perf -->
Baseline `00-baseline` vs `05-linear-scan-levels` (medians of repeated runs; see docs/PERFORMANCE.md for method, variance and every step).

| workload | baseline Mops/s | final Mops/s | speedup | baseline p99 ns | final p99 ns | baseline p99.9 ns | final p99.9 ns | allocs/op before -> after |
|---|---|---|---|---|---|---|---|---|
| passive | 4.79 | 11.53 | 2.41x | 631 | 281 | 1483 | 521 | 0.864 -> 0.000 |
| aggressive | 5.79 | 12.88 | 2.22x | 701 | 301 | 1573 | 551 | 0.764 -> 0.000 |
| balanced | 4.66 | 9.32 | 2.00x | 741 | 381 | 1723 | 771 | 0.811 -> 0.000 |
<!-- END GENERATED:perf -->

Stage 3: benchmark harness (`bench/`, `lob_bench`: seeded passive/aggressive/balanced workloads,
closed-loop TSC per-op latency, separate wall-clock throughput, allocation counter, event checksum,
JSON results with full environment record); report generator (`scripts/make_report.py`) producing
`docs/PERFORMANCE.md` and the generated blocks in README/PROGRESS/DESIGN from `bench/results/*.json`;
five optimisation steps, one commit each, each gated by unit tests plus differential fuzz and
invariant checks: intrusive order lists, node pool, flat id table, contiguous price levels, scan
from the best level. Per-step before/after tables are in `docs/PERFORMANCE.md`. All numbers in this
stage come from those JSON files; none are typed by hand.

## Next
Stage 4: concurrency (SPSC ring, mutex baselines, TSan job). Starts only after approval.

## Known gaps
- Reference model and `OrderBook` share one author and one spec; a misreading of the spec would be
  replicated in both. The hand-checked golden log and the unit tests are the independent check.
- Fuzz prices are confined to a band of roughly 60-140 ticks, quantities to at most 120; extreme
  values are only covered by one unit test.
- No coverage measurement yet.
- The nightly workflow only runs on GitHub's schedule or manual dispatch after it is on `main`;
  it has not been executed yet.
- Performance numbers are from one Windows desktop (not isolated, boost not controlled); treat
  ratios between steps as the result, not absolute ops/s. Linux numbers have not been measured.
- Step 1 (intrusive lists) is not a measurable win by itself; it is kept because the pool builds on it.
- Level lookup by linear scan from the best level is workload dependent (deep books with orders far
  from the touch would favour binary search); only the three bundled workloads were measured.
- The event callback is still a `std::function`; its cost is part of every measurement.
- No self-trade prevention (no participant identity).
- Local builds are MSVC only; gcc/clang are covered by CI.
