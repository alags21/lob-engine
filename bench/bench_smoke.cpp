// Placeholder so the benchmark target builds in CI. Real benchmarks arrive in Stage 3.
#include <benchmark/benchmark.h>

#include "lob/version.hpp"

static void BM_Version(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(lob::version());
}
BENCHMARK(BM_Version);
