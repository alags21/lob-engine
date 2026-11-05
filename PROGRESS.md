# Progress

| Stage | Scope | Status | PR |
|---|---|---|---|
| 0 | Scaffold, CI | in review (CI green) | #1 |
| 1 | Core book | in review (CI green) | #2 |
| 2 | Correctness harness | in review | PR pending |
| 3 | Performance | not started | |
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
model; this is not a throughput benchmark). CI numbers will be added from the Actions logs once the
runs finish. Fuzzing found no bug in the Stage 1 book.

## Next
Stage 3: performance (profile-driven data structure work, benchmark harness with histograms,
before/after table). Starts only after approval.

## Known gaps
- Reference model and `OrderBook` share one author and one spec; a misreading of the spec would be
  replicated in both. The hand-checked golden log and the unit tests are the independent check.
- Fuzz prices are confined to a band of roughly 60-140 ticks, quantities to at most 120; extreme
  values are only covered by one unit test.
- No coverage measurement yet.
- The nightly workflow only runs on GitHub's schedule or manual dispatch after it is on `main`;
  it has not been executed yet.
- Storage is still the naive `std::map` + `std::list` baseline; no measurements taken (Stage 3).
- No self-trade prevention (no participant identity).
- Local builds are MSVC only; gcc/clang are covered by CI.
