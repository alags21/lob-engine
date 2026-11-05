# Progress

| Stage | Scope | Status | PR |
|---|---|---|---|
| 0 | Scaffold, CI | in review | #1 |
| 1 | Core book | not started | |
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

## Next
Stage 1: core order book (awaiting Stage 0 review).

## Known gaps
- Library contains only a version stub; no matching logic yet.
- Linux CI not yet verified from this machine until the PR run completes.

## Measured results
None yet. Numbers appear here only after they are measured and saved under `bench/results/`.
