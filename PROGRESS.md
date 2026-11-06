# Progress

| Stage | Scope | Status | PR |
|---|---|---|---|
| 0 | Scaffold, CI | in review (CI green) | #1 |
| 1 | Core book | in review (CI green) | #2 |
| 2 | Correctness harness | in review (CI green) | #3 |
| 3 | Performance | in review | PR pending |
| 4 | Concurrency | in review | #5 |
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

## Stage 4 headline (generated)

<!-- BEGIN GENERATED:mt -->
Stage 4, producer thread -> queue -> matching thread (medians of repeated runs; latency measured from the intended send time at a fixed paced rate; see docs/CONCURRENCY.md for method, ablations and caveats).

| workload | single-thread Mops/s | SPSC Mops/s | mutex+condvar Mops/s | mutex-book Mops/s | SPSC paced p99 ns | mutex+condvar paced p99 ns | mutex-book paced p99 ns |
|---|---|---|---|---|---|---|---|
| passive | 11.15 | 5.42 | 3.32 | 8.60 | 364113 | 390838 | 175498 |
| aggressive | 12.85 | 6.05 | 3.39 | 9.64 | 371021 | 463838 | 167277 |
| balanced | 9.95 | 5.22 | 3.11 | 8.19 | 412628 | 1893520 | 148974 |

- `passive`: SPSC beats the mutex+condvar queue on throughput (1.64x); does NOT beat the mutex-protected book on throughput (0.63x); reaches 49% of the single-thread ceiling. Paced latency, median: p50 355 ns vs 970 ns for mutex+condvar; p50 355 ns vs 226 ns for mutex-book; p50 355 ns vs 198 ns for single-thread (no hand-off).
  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 187261 ns and was behind schedule for 2.8% of ops. Paced p99 of the other rows: spsc 364113 ns, mutex+condvar 390838 ns, mutex-book 175498 ns; differences at that level are not reliable evidence about the queues.
  - one-slot-per-cache-line variant: 6.02 vs 5.42 Mops/s (1.11x; CV 4.9% / 5.6%).
  - padding ablation: padded 5.42 vs unpadded 4.86 Mops/s (1.12x); padding helped (CV 5.6% / 3.3%).
  - wait strategy throughput: spsc 5.42, spsc-yield 8.13, spsc-backoff 5.70 Mops/s; paced p99: spsc 364113 ns, spsc-yield 1348150 ns, spsc-backoff 1341790 ns.
- `aggressive`: SPSC beats the mutex+condvar queue on throughput (1.79x); does NOT beat the mutex-protected book on throughput (0.63x); reaches 47% of the single-thread ceiling. Paced latency, median: p50 323 ns vs 980 ns for mutex+condvar; p50 323 ns vs 198 ns for mutex-book; p50 323 ns vs 164 ns for single-thread (no hand-off).
  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 113359 ns and was behind schedule for 2.4% of ops. Paced p99 of the other rows: spsc 371021 ns, mutex+condvar 463838 ns, mutex-book 167277 ns; differences at that level are not reliable evidence about the queues.
  - one-slot-per-cache-line variant: 6.60 vs 6.05 Mops/s (1.09x; CV 4.1% / 2.0%).
  - padding ablation: padded 6.05 vs unpadded 5.16 Mops/s (1.17x); padding helped (CV 2.0% / 6.4%).
  - wait strategy throughput: spsc 6.05, spsc-yield 8.01, spsc-backoff 6.09 Mops/s; paced p99: spsc 371021 ns, spsc-yield 2154130 ns, spsc-backoff 441745 ns.
- `balanced`: SPSC beats the mutex+condvar queue on throughput (1.68x); does NOT beat the mutex-protected book on throughput (0.64x); reaches 52% of the single-thread ceiling. Paced latency, median: p50 353 ns vs 988 ns for mutex+condvar; p50 353 ns vs 222 ns for mutex-book; p50 353 ns vs 186 ns for single-thread (no hand-off).
  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 141177 ns and was behind schedule for 2.9% of ops. Paced p99 of the other rows: spsc 412628 ns, mutex+condvar 1893520 ns, mutex-book 148974 ns; differences at that level are not reliable evidence about the queues.
  - one-slot-per-cache-line variant: 5.89 vs 5.22 Mops/s (1.13x; CV 2.7% / 2.5%).
  - padding ablation: padded 5.22 vs unpadded 4.66 Mops/s (1.12x); padding helped (CV 2.5% / 3.6%).
  - wait strategy throughput: spsc 5.22, spsc-yield 7.01, spsc-backoff 5.32 Mops/s; paced p99: spsc 412628 ns, spsc-yield 2507160 ns, spsc-backoff 451755 ns.
<!-- END GENERATED:mt -->

Stage 4: lock-free SPSC ring (`include/lob/spsc_queue.hpp`, acquire/release only, padded indices with
cached remote index; ordering argument in the header and DESIGN.md), bounded mutex+condvar queue
(`mutex_queue.hpp`), shared event/book checksum (`checksum.hpp`), queue unit + stress + pipeline
tests (`tests/test_spsc.cpp`), multi-threaded benchmark `lob_mt_bench` (single-thread ceiling,
mutex-protected book, mutex+condvar queue, SPSC with spin/yield/backoff wait, unpadded and
slot-aligned SPSC variants; with and without the event callback), a TSan CI job, and
`docs/CONCURRENCY.md` generated from `bench/results/stage4/*.json`. Every benchmark run first
asserts that the multi-threaded event stream and final book checksum equal the single-thread run.

## Next
Stage 5: replay and visualizer. Starts only after approval.

## Known gaps
- Core types: this CPU mixes two Windows efficiency classes. Stage 3 results were pinned to a
  lower-class core; Stage 4 results to two cores of the higher class. A re-run of the Stage 3
  baseline and final binaries on a higher-class core is in docs/PERFORMANCE.md ("Core-type check").
  Core selection in the benchmarks is positional, not by class; it happened to land on the higher class for Stage 4.
- Stage 4: tail latency (p99 and above) in the paced run is dominated by OS interference on this
  non-isolated desktop (even the no-queue row shows it); only medians are a reliable queue comparison.
- Stage 4: the two pinned cores are different physical cores but their core type (this CPU may mix
  core types) and the TSC offset (estimated by ping-pong only) are not fully controlled.
- Stage 4: TSan has not been run on Windows (unavailable) and was not shown to catch a deliberately
  injected ordering bug; the book itself is single-threaded and not thread-safe.
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
