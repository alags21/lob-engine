# Progress

Every number in this file comes from a generated block (`scripts/make_report.py`,
`scripts/make_mt_report.py`) built from saved result files. Test and CI parameters are in the
workflow files, not repeated here.

| Stage | Scope | Status | PR |
|---|---|---|---|
| 0 | Scaffold, CI | in review (CI green) | #1 |
| 1 | Core book | in review (CI green) | #2 |
| 2 | Correctness harness | in review (CI green) | #3 |
| 3 | Performance | in review (CI green) | #4 |
| 4 | Concurrency | in review (CI green) | #5 |
| 5 | Replay and visualizer | in review (CI green) | #6 |
| 6 | Polish | in review (CI green) | #7 |
| 7 | Real ITCH data validation | **IN PROGRESS, interrupted (usage limit)** | #8 |

Nothing is merged yet. Merge order and tagging: [docs/RELEASE.md](docs/RELEASE.md).

## Built
- **Stage 0**: CMake project (library, tests, bench, tools), clang-format, GitHub Actions (gcc and
  clang, Release and Debug, ASan+UBSan), local Windows toolchain notes in `docs/BENCHMARKING.md`
  (`scripts/win-build.cmd`).
- **Stage 1**: `lob::OrderBook`: limit, market, IOC, FOK, cancel, modify; price-time priority; trades at
  the maker price; synchronous event callback; integer ticks and lots; GoogleTest unit tests for every
  order type and edge case.
- **Stage 2**: slow reference model (`tests/reference/`), seeded differential fuzzer (`lob_fuzz`),
  `OrderBook::validate()` invariants checked after every call in non-NDEBUG builds, text op-log and
  event formats with a hand-checked golden log (`docs/EVENT_FORMAT.md`, `lob_replay`), CI fuzz jobs and a
  nightly fuzz workflow.
- **Stage 3**: benchmark harness (seeded passive/aggressive/balanced workloads, TSC per-op latency,
  wall-clock throughput, allocation counter, event checksum, JSON results with environment record),
  five optimisation steps one commit each, gated by the correctness suite, and generated reports.
- **Stage 4**: lock-free SPSC ring (acquire/release only, padded indices, cached remote index),
  mutex+condvar queue baseline, multi-threaded benchmark with a checksum gate against the
  single-thread run, stress tests, ThreadSanitizer CI job with a negative control.
- **Stage 5**: seeded synthetic market (`lob_sim`), CSV/JSON-lines/oplog replay (`lob_replay`),
  `lob-run-v1` JSON export with events and book snapshots, and the static offline visualizer in `viz/`.
- **Stage 6**: README with architecture diagram and generated tables, DESIGN tradeoffs and limitations,
  `docs/RELEASE.md`, keyboard-reachable "Load run" control, consistency pass over generated numbers.

## Stage 7 (real Nasdaq ITCH data): UNFINISHED
Work stopped at a usage limit. Done: ITCH 5.0 parser/encoder, stream-decoding validator (order-level,
priority, reference cross-check), synthetic ITCH generator, CI job on synthetic data only, repo data guard,
real-file scan, exploratory probes. Not done: full-day validation, hypothesis experiments, replay benchmark,
generated tables. Everything needed to resume, with exact commands, is in `docs/REALDATA.md` (section
TODO). Raw data stays in the local folder `Data/itch` under the user profile, outside the repository.

## Measured results (generated)

### Correctness fuzzing

<!-- BEGIN GENERATED:fuzz -->
Differential fuzz against the reference model (all with zero mismatches; every line is a PASS line saved in `docs/results/`):

- local, MSVC checked build: 3 run(s) from seed 1, 3,000,000 ops, 4,601,135 events, 1,301,514 trades, largest book 4,928 orders
- CI, Stage 2 PR run: ASan + UBSan, 1 run(s) from seed 1, 200,000 ops, 305,553 events, 85,122 trades, largest book 1,157 orders
- CI, Stage 2 PR run: gcc Release, 2 run(s) from seed 1, 1,000,000 ops, 1,530,900 events, 430,010 trades, largest book 2,538 orders
- CI, Stage 2 PR run: gcc Release, 1 run(s) from seed 100005, 500,000 ops, 764,848 events, 214,644 trades, largest book 2,437 orders
- CI, Stage 2 PR run: clang Debug, 1 run(s) from seed 1, 100,000 ops, 152,500 events, 42,283 trades, largest book 767 orders
- CI, Stage 2 PR run: gcc Debug, 1 run(s) from seed 1, 100,000 ops, 152,500 events, 42,283 trades, largest book 767 orders
- CI, Stage 2 PR run: clang Release, 2 run(s) from seed 1, 1,000,000 ops, 1,530,900 events, 430,010 trades, largest book 2,538 orders
- CI, Stage 2 PR run: clang Release, 1 run(s) from seed 100005, 500,000 ops, 764,848 events, 214,644 trades, largest book 2,437 orders
<!-- END GENERATED:fuzz -->

### Single-thread book (Stage 3)

<!-- BEGIN GENERATED:perf -->
Baseline `00-baseline` vs `05-linear-scan-levels` (medians of repeated runs; see docs/PERFORMANCE.md for method, variance and every step).

| workload | baseline Mops/s | final Mops/s | speedup | baseline p99 ns | final p99 ns | baseline p99.9 ns | final p99.9 ns | allocs/op before -> after |
|---|---|---|---|---|---|---|---|---|
| passive | 4.79 | 11.53 | 2.41x | 631 | 281 | 1483 | 521 | 0.864 -> 0.000 |
| aggressive | 5.79 | 12.88 | 2.22x | 701 | 301 | 1573 | 551 | 0.764 -> 0.000 |
| balanced | 4.66 | 9.32 | 2.00x | 741 | 381 | 1723 | 771 | 0.811 -> 0.000 |
<!-- END GENERATED:perf -->

### Queue designs (Stage 4)

<!-- BEGIN GENERATED:mt -->
Stage 4, producer thread -> queue -> matching thread (medians of repeated runs; latency measured from the intended send time at a fixed paced rate; see docs/CONCURRENCY.md for method, ablations and caveats).

| workload | single-thread Mops/s | SPSC Mops/s | mutex+condvar Mops/s | mutex-book Mops/s | SPSC paced p99 ns | mutex+condvar paced p99 ns | mutex-book paced p99 ns |
|---|---|---|---|---|---|---|---|
| passive | 11.15 | 5.42 | 3.32 | 8.60 | 364113 | 390838 | 175498 |
| aggressive | 12.85 | 6.05 | 3.39 | 9.64 | 371021 | 463838 | 167277 |
| balanced | 9.95 | 5.22 | 3.11 | 8.19 | 412628 | 1893520 | 148974 |

- `passive`: SPSC is 1.64x the mutex+condvar queue (a win) and 0.63x the mutex-protected book (a LOSS: the lock-only design with no hand-off is faster); it reaches 49% of the single-thread ceiling. Paced median latency 355 ns vs 970 ns (mutex+condvar) and 226 ns (mutex-book).
- `aggressive`: SPSC is 1.79x the mutex+condvar queue (a win) and 0.63x the mutex-protected book (a LOSS: the lock-only design with no hand-off is faster); it reaches 47% of the single-thread ceiling. Paced median latency 323 ns vs 980 ns (mutex+condvar) and 198 ns (mutex-book).
- `balanced`: SPSC is 1.68x the mutex+condvar queue (a win) and 0.64x the mutex-protected book (a LOSS: the lock-only design with no hand-off is faster); it reaches 52% of the single-thread ceiling. Paced median latency 353 ns vs 988 ns (mutex+condvar) and 222 ns (mutex-book).

Tail latency (p99 and above) on this machine is dominated by OS interference; see docs/CONCURRENCY.md.
<!-- END GENERATED:mt -->

Full tables, environment records and variance: `docs/PERFORMANCE.md`, `docs/CONCURRENCY.md`.

## Known gaps
- **License**: no LICENSE file has been added (the owner's choice). One is required before the
  repository goes public.
- **Platform**: all timings are from one Windows desktop (not isolated, boost not controlled). Treat
  ratios between steps as the result, not absolute ops/s. Linux timings have not been measured; Linux
  is covered by CI for correctness and sanitizers only.
- **Core types**: the CPU mixes two Windows efficiency classes. Stage 3 results were pinned to a
  lower-class core and Stage 4 to two higher-class cores. A re-run of the Stage 3 baseline and final
  binaries on a higher-class core is in `docs/PERFORMANCE.md` ("Core-type check"). Core selection in
  the benchmarks is positional, not by class.
- **Tail latency**: p99 and above in the paced multi-threaded runs is dominated by OS interference on
  this desktop (even the no-queue row shows it); only medians are a reliable queue comparison.
- **Stage 3 step 1** (intrusive lists) is not a measurable win by itself; it is kept because the pool
  builds on it. **Step 5** (level lookup by scanning from the best level) is workload dependent: a very
  deep book with orders far from the touch would favour binary search.
- **Event callback** is a `std::function`; its cost is part of every measurement.
- **Reference model** and `OrderBook` share one author and one spec; a misreading of the spec would be
  replicated in both. The hand-checked golden log and the unit tests are the independent check.
- **Fuzz coverage**: prices are confined to a narrow band and quantities to small values; extreme
  values are covered by a unit test only. No coverage measurement.
- **Nightly fuzz workflow** has never run (it only runs from the default branch).
- **Self-trade prevention**: none (the book has no participant identity).
- **TSan** is Linux CI only (unavailable with MSVC); a negative control shows it can fail. The book
  itself is single-threaded and not thread-safe.
- **Visualizer**: checked in one real browser (Edge headless, dark theme, desktop width, plus a manual
  look in another browser pane by the project owner's assistant) and by Node mock-canvas tests;
  Firefox/Safari, the light theme, narrow screens and touch are untested. The book animates at
  snapshot granularity, not per op.
- **Pages workflow** is manual-only and has never run; Pages is not enabled.
- **Local builds** are MSVC only; gcc and clang are covered by CI.

## Next
Open: merge the stacked PRs in order and tag `v0.1.0` (see `docs/RELEASE.md`), after the owner decides
on a license and on enabling Pages.
