#pragma once

// Synthetic market generator and run recorder (Stage 5). No external or recorded market data is
// used anywhere: every order is produced from a seeded RNG.
//
// Model (all parameters are in MarketConfig and are written into the exported run):
//  * Arrivals: a Poisson process, i.e. exponentially distributed gaps with mean 1e9 / rate ns.
//  * A hidden "fundamental" price takes a +-1 tick step with probability `drift_prob` per op.
//    Passive limit orders are placed `1 + d` ticks away from it on their own side, where d follows
//    `depth_dist` (geometric, uniform, or a two-scale fat tail), so the book builds around it and
//    follows it.
//  * Each arrival is a cancel (cancel_fraction), a modify (modify_fraction) or a submit. A submit
//    is aggressive with probability `aggressiveness`: 50% IOC limit crossing the touch by 0-2
//    ticks, 30% market IOC, 10% FOK limit, 10% marketable GTC limit. Other submits are passive
//    GTC limits.
//  * Cancels and modifies target an order that is really resting in the book (the generator reads
//    the book), so they succeed; open orders are capped softly around `book_target`.
//
// Determinism: only integer arithmetic and IEEE + - * / are used (no libm), so a given config
// yields the identical run on every platform and compiler. The exponential gap uses a private
// series log() for that reason.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lob/checksum.hpp"
#include "lob/event_log.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob::sim {

enum class DepthDist : std::uint8_t { Geometric, Uniform, FatTail };

struct MarketConfig {
  std::uint64_t seed = 1;
  std::uint64_t ops = 10000;
  double arrival_rate_per_sec = 100000.0;  ///< mean arrivals per second (Poisson)
  double cancel_fraction = 0.30;
  double modify_fraction = 0.08;
  double aggressiveness = 0.15;  ///< fraction of submits that are marketable
  DepthDist depth_dist = DepthDist::Geometric;
  int depth_mean_ticks = 6;  ///< mean extra distance from the fundamental (uniform: max is 2x mean)
  Price initial_mid = 10000;
  double drift_prob = 0.05;  ///< probability per op that the fundamental moves one tick
  int max_qty = 20;
  std::uint64_t book_target = 300;  ///< soft cap on resting orders
};

struct TimedOp {
  std::uint64_t t_ns = 0;
  Op op;
};

struct TimedEvent {
  std::uint64_t op_index = 0;
  Event ev;
};

struct Snapshot {
  std::uint64_t op_index = 0;  ///< state after this many ops have been applied
  std::uint64_t t_ns = 0;
  std::vector<LevelInfo> bids;  ///< best first
  std::vector<LevelInfo> asks;  ///< best first
};

struct Run {
  MarketConfig config;
  std::size_t snapshot_every = 10;
  std::size_t depth_levels = 15;
  std::vector<TimedOp> ops;
  std::vector<TimedEvent> events;
  std::vector<Snapshot> snapshots;
  std::string source = "simulated";  ///< "simulated" or "replay"
  std::uint64_t checksum = 0;  ///< event + final book digest (lob/checksum.hpp)
};

/// Applies timed ops to an OrderBook and records the events and periodic snapshots.
class Recorder {
 public:
  Recorder(std::size_t snapshot_every, std::size_t depth_levels);
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  void apply_op(const TimedOp& op);
  /// Records the final snapshot (if not already taken) and the checksum.
  Run finish(const MarketConfig& cfg);
  const OrderBook& book() const { return *book_; }

 private:
  void snapshot();
  Run run_;
  std::unique_ptr<OrderBook> book_;
  EventHasher hasher_;
};

/// Generates and records a synthetic run.
Run simulate(const MarketConfig& cfg, std::size_t snapshot_every = 10, std::size_t depth_levels = 15);

const char* to_string(DepthDist d);
bool parse_depth_dist(const std::string& s, DepthDist& out);

}  // namespace lob::sim
