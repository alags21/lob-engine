# Toolchain and benchmarking

## Local toolchain (Windows 11, no system C++ compiler needed beyond MSVC Build Tools)

`conda create -n lob -c conda-forge cmake ninja cxx-compiler gtest benchmark clang-format`
installs the build tools and libraries. On Windows, conda-forge's `cxx-compiler` is only a
wrapper (`vs2022_win-64`): it needs a real MSVC. This machine already had Visual Studio
Build Tools (MSVC 14.50, `cl.exe` under
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`), so that is the compiler used.

Versions installed by the command above (from `conda list -n lob`): cmake 4.4.5, gtest 1.18.0,
benchmark 1.10.0, clang-format 23.1.4.

Build and test:

```
scripts\win-build.cmd build
```

The script loads `vcvars64.bat`, puts the env's `Library\bin` on PATH, configures with Ninja in
Release, builds, and runs ctest. conda's gtest/benchmark are built against the release CRT, so
local Windows builds use Release (not Debug). Debug and sanitizer builds run in CI on Linux.

## CI toolchain
Ubuntu 24.04, gcc and clang from apt, `libgtest-dev`, `libbenchmark-dev`, Ninja, CMake from apt.

## Benchmark methodology (Stage 3)
Harness: `bench/bench_main.cpp` (`lob_bench`), workloads in `bench/workload.hpp`, driver
`scripts/run_bench.py`, interleaved multi-commit driver `scripts/remeasure.py`, report generator
`scripts/make_report.py`. Results are JSON files in `bench/results/`; every table in the docs is
generated from them.

- Build for benchmarks: Release (`/O2 /Ob2 /DNDEBUG` on MSVC), not the checked build. Run
  `scripts\win-build.cmd build-rel`, then `python scripts/run_bench.py --label NAME`.
- Closed-loop, single thread: ops are issued back to back and each op's latency is its service
  time. Because nothing queues behind a slow op, coordinated omission does not arise, but the
  numbers do not describe behaviour under an open-loop arrival process or contention.
- Clock: x86 TSC read with `lfence` before and after (`rdtsc`), converted to ns by calibrating
  against `steady_clock` for 200 ms. The cost of one timestamp pair is measured and written to each
  result file; it is **not** subtracted, so small latencies include it.
- Throughput is measured separately from latency: a pass with no per-op timers (wall time of the
  loop), so timestamping does not distort it. Latency percentiles are exact (all samples sorted),
  per repetition; the files also contain a log2 histogram.
- One untimed warmup repetition, then the configured number of measured repetitions, each on a
  freshly constructed book. Reported figures are medians over repetitions; min, max and the
  coefficient of variation are stored.
- The thread is pinned to one logical CPU and the process priority is raised. Turbo/boost, power
  plan and background load are not controlled on this desktop; the CPU load measured just before
  each run is recorded.
- An FNV checksum over every emitted event plus the final book on the identical op stream must
  match between steps; a mismatch makes `make_report.py` fail.
- CI only checks that the benchmark builds and runs (tiny op count); CI timings are never used.
