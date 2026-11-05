# Implementation plan: lob-engine

A C++20 limit order book and matching engine with a correctness-first test strategy,
reproducible latency/throughput benchmarks, and a browser visualizer for demos.

## Ground rules
- **Every number is measured.** Benchmarks write `bench/results/*.json` with hardware, compiler,
  flags, commit SHA and run count. README tables are generated from those files. No hand-typed
  numbers, no invented results. If a stage cannot be measured, say so in `PROGRESS.md`.
- One branch and one PR per stage. Tests and CI green before a stage is called done.
- Stage N+1 starts only after approval from the project owner.
- `PROGRESS.md` is updated in every stage PR: what was built, what is next, known gaps.

## Toolchain (this machine is Windows, no compiler preinstalled)
Preferred: conda-forge env (`conda create -n lob -c conda-forge cmake ninja cxx-compiler gtest benchmark`)
or any equivalent. CI builds on Linux (gcc and clang). Record the exact toolchain in `docs/BENCHMARKING.md`.
Benchmark numbers must note OS, CPU, compiler, flags, and whether turbo/background load was controlled.

## Stages

### Stage 0: scaffold
CMake project (library `lob`, tests, benchmarks, tools), clang-format, `.gitignore`,
GitHub Actions (Linux gcc + clang, Release + Debug, ASan/UBSan job), README skeleton,
`PROGRESS.md`, `docs/DESIGN.md` skeleton. PR: `stage-0-scaffold`.

### Stage 1: core book
Order types: limit, market, IOC, FOK, cancel, modify (cancel/replace loses priority unless size reduces).
Price-time priority, partial fills, trade events with maker/taker ids and prices (maker price rules).
Public API: `submit`, `cancel`, `modify`, event callback or event buffer. Integer prices (ticks) and
integer quantities only. Unit tests (GoogleTest) for every order type and edge case. PR: `stage-1-core`.

### Stage 2: correctness harness
- Slow, obviously-correct reference model (`std::map` + `std::deque`) in `tests/reference/`.
- Differential fuzzing: random order streams (seeded, reproducible) through both, compare every
  event and the final book. Minimum 1M-event fuzz run in CI (shorter) and nightly (longer).
- Invariants checked after every event in debug builds: no crossed book, no zero-qty resting orders,
  FIFO within level, level aggregates match orders.
- Deterministic event-log record/replay format (`docs/EVENT_FORMAT.md`). PR: `stage-2-correctness`.

### Stage 3: performance
- Data structure work driven by profiling, not guessing: contiguous price-level storage, intrusive
  order lists, object pool/arena, avoid allocation on the hot path, id-to-order lookup.
- Benchmark harness: single-thread submit/cancel/modify mix with realistic ratios; reports
  throughput and p50/p99/p99.9/max per-operation latency from a histogram (cycle counter or steady_clock,
  state which). Warmup, pinned thread where the OS allows, N repetitions with variance.
- Keep a before/after table: naive implementation vs each optimization, each measured. PR: `stage-3-performance`.

### Stage 4: concurrency
Lock-free SPSC ring buffer (acquire/release, cache-line padded indices) feeding a single matching thread.
Compare against a mutex+condvar queue and a mutex-protected book on the same workload. ThreadSanitizer
CI job and a stress test. Report throughput and tail latency for each. PR: `stage-4-concurrency`.

### Stage 5: replay and visualizer
- Synthetic market generator (configurable arrival, cancel and aggressiveness rates) and a CSV/JSON
  replay driver. Optional adapter for a public sample feed if licensing allows; otherwise synthetic only.
- Export a JSON event stream plus periodic book snapshots.
- Visualizer: static site in `viz/` (plain HTML/JS/canvas, no build step) that loads an exported run:
  animated depth chart, trade tape, spread/mid-price over time, per-operation latency histogram.
  Playback controls and a sample run committed so it works without building C++.
  Publishable on GitHub Pages. PR: `stage-5-visualizer`.

### Stage 6: polish
README with architecture diagram, generated benchmark tables and a GIF/screenshot of the visualizer,
`docs/DESIGN.md` with tradeoffs and what was rejected, honest "limitations" section, tag `v0.1.0`.
PR: `stage-6-polish`.

## Metrics to track (all from measured runs)
| Metric | Where |
|---|---|
| Orders/sec (single thread, mixed workload) | `bench/results/` |
| p50 / p99 / p99.9 / max latency per op | `bench/results/` |
| Differential fuzz events passed, seeds | CI log + `PROGRESS.md` |
| Sanitizer status (ASan, UBSan, TSan) | CI |
| SPSC vs mutex queue throughput and tail | `bench/results/` |
| Test count and coverage | CI |

## Resume bullets this should support (only after measured)
- Matching engine sustaining [X]M orders/s with p99 [X] ns.
- Verified against a reference model with [X] fuzzed events.
- (Stretch) Lock-free SPSC queue cut p99 by [X]% versus a mutex queue.
