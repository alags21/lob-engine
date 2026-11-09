#pragma once

// Seeded differential fuzzer: random op streams run through lob::OrderBook and the reference
// model; every returned status, every emitted Event, and the full book state are compared after
// each op, and OrderBook::validate() is checked after each op.

#include <cstdint>
#include <string>
#include <vector>

#include "lob/event_log.hpp"

namespace lob::fuzz {

struct FuzzConfig {
  std::uint64_t seed = 1;
  std::uint64_t ops = 100000;
  /// Full book-state comparison and OrderBook::validate() run every this many ops (and always
  /// after the last op). Returned statuses and emitted events are compared on every op.
  std::uint64_t state_check_interval = 1;
};

struct FuzzResult {
  bool ok = true;
  std::string message;  ///< On failure: what differed, with seed and op index.
  std::uint64_t ops = 0;
  std::uint64_t events = 0;  ///< Events emitted by the book under test (all kinds).
  std::uint64_t trades = 0;
  std::uint64_t rejects = 0;
  std::size_t max_resting_orders = 0;
  std::vector<Op> failing_prefix;  ///< All ops up to and including the failing one.
};

/// Deterministic op generator: same seed, same stream, on every platform.
class OpGenerator {
 public:
  explicit OpGenerator(std::uint64_t seed);
  Op next();

 private:
  std::uint64_t rnd();
  std::uint64_t below(std::uint64_t n) { return rnd() % n; }
  struct Known {
    OrderId id;
    Price price;
  };
  std::uint64_t state_;
  OrderId next_id_ = 1;
  Price mid_ = 100;
  std::vector<Known> open_;   // GTC limits issued and not yet cancelled (may have filled)
  std::vector<Known> known_;  // recent ids (live or not), ring buffer
  std::size_t ring_pos_ = 0;
  std::uint64_t count_ = 0;
};

FuzzResult run_fuzz(const FuzzConfig& cfg);

/// Writes the op log (docs/EVENT_FORMAT.md) for `ops` to `path`. Returns false on I/O failure.
bool write_op_log(const std::string& path, const std::vector<Op>& ops, std::uint64_t seed);

}  // namespace lob::fuzz
