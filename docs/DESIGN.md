# Design

Skeleton. Filled in per stage; final tradeoffs in Stage 6.

## Layout
- `include/lob/`: public headers.
- `src/`: library implementation (`lob` target).
- `tests/`: GoogleTest unit tests.
- `bench/`: Google Benchmark targets (placeholder until Stage 3).
- `tools/`: command-line tools (placeholder).

## Build options
`LOB_BUILD_TESTS`, `LOB_BUILD_BENCH`, `LOB_BUILD_TOOLS`, `LOB_SANITIZE` (ASan+UBSan), `LOB_WERROR`.

## Decisions
(none yet)
