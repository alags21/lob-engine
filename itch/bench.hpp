#pragma once

#include <cstdint>
#include <string>

namespace lob::itch {

struct BenchArgs {
  std::string input, json, symbols;  // symbols: comma separated
  std::string label = "realdata", commit = "unknown", os = "unknown", flags = "unknown", note;
  std::uint64_t reps = 7, max_messages = 0;
  int cpu = -1;
  double load = -1;
};

/// Decodes the ITCH stream once (decompression, parsing, validation bookkeeping, engine) while
/// recording every operation applied to the engine for the selected symbols, then replays that
/// in-memory op stream through fresh books with the Stage 3 methodology (warmup, N repetitions,
/// separate throughput and per-op latency passes, pinned thread) and writes a JSON result.
int run_bench(const BenchArgs& a);

}  // namespace lob::itch
