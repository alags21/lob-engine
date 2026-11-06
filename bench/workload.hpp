#pragma once

// Seeded, reproducible synthetic order flow for benchmarks. Generation happens before timing; the
// timed loops only replay a pre-built vector of ops.
//
// Model (all knobs are in WorkloadConfig and are written to the result JSON):
//  - Op mix: submit_pct / cancel_pct / modify_pct (sum to 100) are *targets*. Book size is
//    regulated around `book_target` resting orders by turning an excess passive submit into a
//    cancel of an open order, so the realised mix (reported in the JSON) differs slightly.
//  - Submits are passive (limit GTC resting `1 + geometric(depth_mean)` ticks away from mid on
//    their own side) or, with probability aggressive_pct, aggressive: 50% IOC limits that cross
//    by 0-3 ticks, 25% market orders (IOC), 15% FOK limits, 10% marketable GTC limits.
//  - Cancels target a uniformly random order the generator believes is open (it may already have
//    been filled, which exercises the unknown-id path); modifies pick the same way: 50% size
//    reduction at the same price, 25% size increase, 25% price move of 1-3 ticks.
//  - Mid price takes a +-1 tick random walk every 64 ops inside [mid0-40, mid0+40].
//  - Quantities are 1-20 lots, with 10% in 1-200.

#include <cstdint>
#include <string>
#include <vector>

#include "lob/event_log.hpp"

namespace lob::bench {

struct WorkloadConfig {
  std::string name;
  std::uint64_t seed = 1;
  std::uint64_t ops = 2'000'000;
  int submit_pct = 50;
  int cancel_pct = 40;
  int modify_pct = 10;
  int aggressive_pct = 5;  ///< percent of submits that are aggressive
  int depth_mean = 8;      ///< mean extra ticks away from mid for passive orders
  std::uint64_t book_target = 2000;
  Price mid0 = 10000;
};

inline WorkloadConfig preset(const std::string& name) {
  WorkloadConfig c;
  c.name = name;
  if (name == "passive") {  // passive-heavy: mostly resting orders and cancels
    c.submit_pct = 50; c.cancel_pct = 40; c.modify_pct = 10; c.aggressive_pct = 5;
  } else if (name == "aggressive") {  // aggressive-heavy: lots of crossing flow and sweeps
    c.submit_pct = 70; c.cancel_pct = 20; c.modify_pct = 10; c.aggressive_pct = 55;
    c.book_target = 1000;
  } else if (name == "balanced") {
    c.submit_pct = 55; c.cancel_pct = 30; c.modify_pct = 15; c.aggressive_pct = 25;
  }
  return c;
}

struct Realised {
  std::uint64_t submits = 0, cancels = 0, modifies = 0;
};

namespace detail {
struct Rng {
  std::uint64_t s;
  std::uint64_t next() {  // splitmix64
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
  // Geometric-ish: number of failures before a success with p = 1/mean.
  std::uint64_t geometric(std::uint64_t mean) {
    std::uint64_t k = 0;
    while (k < 10 * mean && below(mean) != 0) ++k;
    return k;
  }
};
}  // namespace detail

inline std::vector<Op> generate(const WorkloadConfig& cfg, Realised* realised = nullptr) {
  detail::Rng rng{cfg.seed};
  std::vector<Op> ops;
  ops.reserve(cfg.ops);
  struct Open {
    OrderId id;
    Price price;
    Side side;
    Qty qty;
  };
  std::vector<Open> open;
  OrderId next_id = 1;
  Price mid = cfg.mid0;
  Realised r;

  auto qty = [&]() -> Qty {
    return rng.below(10) == 0 ? static_cast<Qty>(1 + rng.below(200)) : static_cast<Qty>(1 + rng.below(20));
  };
  auto make_cancel = [&]() {
    Op op;
    op.kind = Op::Kind::Cancel;
    if (open.empty()) {
      op.id = 1 + rng.below(next_id);  // stale id
    } else {
      const std::size_t i = static_cast<std::size_t>(rng.below(open.size()));
      op.id = open[i].id;
      open[i] = open.back();
      open.pop_back();
    }
    ++r.cancels;
    return op;
  };

  for (std::uint64_t n = 0; n < cfg.ops; ++n) {
    if (n % 64 == 63) {
      const std::int64_t step = static_cast<std::int64_t>(rng.below(3)) - 1;
      mid += step;
      if (mid < cfg.mid0 - 40) mid = cfg.mid0 - 40;
      if (mid > cfg.mid0 + 40) mid = cfg.mid0 + 40;
    }
    const int dice = static_cast<int>(rng.below(100));
    if (dice < cfg.submit_pct) {
      Op op;
      op.kind = Op::Kind::Submit;
      op.id = next_id++;
      op.side = rng.below(2) ? Side::Buy : Side::Sell;
      op.qty = qty();
      const bool aggressive = static_cast<int>(rng.below(100)) < cfg.aggressive_pct;
      if (!aggressive) {
        if (open.size() > cfg.book_target + cfg.book_target / 2) {  // regulate book size
          --next_id;
          ops.push_back(make_cancel());
          continue;
        }
        const Price dist = 1 + static_cast<Price>(rng.geometric(static_cast<std::uint64_t>(cfg.depth_mean)));
        op.type = OrderType::Limit;
        op.tif = TimeInForce::GTC;
        op.price = op.side == Side::Buy ? mid - dist : mid + dist;
        open.push_back({op.id, op.price, op.side, op.qty});
      } else {
        const Price cross = static_cast<Price>(rng.below(4));
        const Price px = op.side == Side::Buy ? mid + 1 + cross : mid - 1 - cross;
        const int kind = static_cast<int>(rng.below(100));
        if (kind < 50) {
          op.type = OrderType::Limit; op.tif = TimeInForce::IOC; op.price = px;
        } else if (kind < 75) {
          op.type = OrderType::Market; op.tif = TimeInForce::IOC; op.price = 0;
        } else if (kind < 90) {
          op.type = OrderType::Limit; op.tif = TimeInForce::FOK; op.price = px;
        } else {
          op.type = OrderType::Limit; op.tif = TimeInForce::GTC; op.price = px;
          open.push_back({op.id, op.price, op.side, op.qty});
        }
      }
      ++r.submits;
      ops.push_back(op);
    } else if (dice < cfg.submit_pct + cfg.cancel_pct) {
      ops.push_back(make_cancel());
    } else {
      Op op;
      op.kind = Op::Kind::Modify;
      if (open.empty()) {
        op.id = 1 + rng.below(next_id);
        op.price = mid;
        op.qty = qty();
      } else {
        Open& o = open[static_cast<std::size_t>(rng.below(open.size()))];
        op.id = o.id;
        const int k = static_cast<int>(rng.below(4));
        if (k < 2) {
          op.price = o.price;
          op.qty = o.qty > 1 ? static_cast<Qty>(1 + rng.below(static_cast<std::uint64_t>(o.qty))) : 1;
        } else if (k == 2) {
          op.price = o.price;
          op.qty = o.qty + 1 + static_cast<Qty>(rng.below(20));
        } else {
          const Price d = 1 + static_cast<Price>(rng.below(3));
          op.price = o.price + (rng.below(2) ? d : -d);
          if (op.price < 1) op.price = 1;
          op.qty = o.qty;
        }
        o.price = op.price;
        o.qty = op.qty;
      }
      ++r.modifies;
      ops.push_back(op);
    }
  }
  if (realised) *realised = r;
  return ops;
}

}  // namespace lob::bench
