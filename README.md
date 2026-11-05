# lob-engine

C++20 limit order book and matching engine with differential-fuzz verification,
reproducible benchmarks, and a browser visualizer.

Status: early development (see [PROGRESS.md](PROGRESS.md)). Plan: [docs/PLAN.md](docs/PLAN.md).

## Build

Linux/macOS:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake 3.20+, a C++20 compiler, GoogleTest and Google Benchmark.
Windows: see [docs/BENCHMARKING.md](docs/BENCHMARKING.md).
