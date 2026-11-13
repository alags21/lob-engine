# lob-engine

C++20 limit order book + matching engine. Correctness-first: reference model, differential fuzzing,
reproducible benchmarks, SPSC pipeline, static browser visualizer, real Nasdaq ITCH validation.
Start with `docs/OVERVIEW.md`, then `README.md`, `docs/DESIGN.md` (decisions, limitations) and `docs/RESULTS.md` (stages, known gaps).

## Build and test (Linux)
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure      # 108 tests, ~6s
build/tests/lob_fuzz --seed 1 --ops 1000000 --runs 2
node viz/test/viz_test.js                       # visualizer tests
```
- Debug build (no `NDEBUG`) runs `OrderBook::validate()` after every mutating call.
- Sanitizers: `-DLOB_SANITIZE=ON` (ASan+UBSan) or `-DLOB_TSAN=ON`, with `-DCMAKE_BUILD_TYPE=Debug`.
- Deps: cmake, ninja, g++/clang++ (+ libclang-rt-18-dev for sanitizers), libgtest-dev, libbenchmark-dev, zlib1g-dev (installed by the SessionStart hook).

## Layout
- `include/lob/`, `src/` – library (order book, event log, sim, replay IO, ITCH, SPSC/mutex queues)
- `tests/` – unit, fuzz, `reference/` slow model; `tools/` – `lob_sim`, `lob_replay`; `itch/` – ITCH tools
- `bench/`, `scripts/` – benchmarks and report generators; `viz/` – static visualizer; `docs/`

## Conventions
- Style: `.clang-format` (Google, 100 cols, 2-space). Warnings are on (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`).
- The book is single-threaded and deterministic; concurrency stays outside it. Integer ticks/lots only.
- Any engine change must keep the differential fuzz vs the reference model passing.
- Tables in README/docs between `<!-- BEGIN GENERATED:... -->` markers are generated; never hand-edit.
  Regenerate with `python scripts/make_report.py` / `make_mt_report.py` (`--check` to verify).
- Never quote benchmark numbers without their method/caveats; Linux timings here are not the recorded ones
  (those are from one Windows desktop). Never commit raw market data (see `.gitignore`);
  `scripts/hooks/pre-commit` runs `scripts/check_repo_size.py --staged`.

## Workflow for this session
- Develop on branch `claude/tender-darwin-wms32f`; commit and push there. No PRs unless asked.
- The cloud container is ephemeral: push work you want to keep.
