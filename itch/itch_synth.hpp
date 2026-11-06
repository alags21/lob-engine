#pragma once

// Generates a small synthetic ITCH 5.0 file from our own market simulator, so CI and the unit tests
// can exercise the parser, validator and replay path without any Nasdaq data. The stream is
// internally consistent by construction (it is derived from our engine's own behaviour), so it can
// show that the plumbing works, not that the engine agrees with a real exchange: that is what the
// real-data run is for.

#include <cstdint>
#include <vector>

#include "lob/itch.hpp"

namespace lob::itch {

struct SynthConfig {
  std::uint64_t seed = 1;
  std::uint64_t ops_per_symbol = 4000;
  int symbols = 2;
  std::uint64_t base_ts_ns = 34'200ULL * 1'000'000'000ULL;  // 09:30:00
};

/// Framed ITCH bytes: system events, stock directory, trading actions, opening crosses, order flow
/// of all symbols interleaved by time, closing crosses, end-of-day system events. Symbols are named
/// SYNA, SYNB, ...
std::vector<std::uint8_t> make_synthetic_itch(const SynthConfig& cfg);

}  // namespace lob::itch
