# Progress

| Stage | Scope | Status | PR |
|---|---|---|---|
| 0 | Scaffold, CI | in review (CI green) | #1 |
| 1 | Core book | in review | #2 |
| 2 | Correctness harness | not started | |
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

## Next
Stage 2: reference model (`std::map` + `std::deque`), differential fuzzing, invariant checks,
event-log record/replay format. Starts only after approval.

## Known gaps
- Storage is `std::map` + `std::list` + `std::unordered_map`: correct but allocation-heavy; Stage 3
  replaces it driven by profiling.
- No self-trade prevention (the book has no participant identity).
- Only one platform compiled locally (MSVC); gcc/clang are covered by CI only.
- Invariant checker and fuzzing do not exist yet (Stage 2); correctness is currently from unit tests only.
- No measurements taken yet.

## Measured results
None yet. Numbers appear here only after they are measured and saved under `bench/results/`.
