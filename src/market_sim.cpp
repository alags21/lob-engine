#include "lob/market_sim.hpp"

#include <algorithm>

namespace lob::sim {

namespace {

struct Rng {
  std::uint64_t s;
  std::uint64_t next() {  // splitmix64
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
  // True with probability p_micro / 1e6.
  bool chance(std::uint64_t p_micro) { return below(1000000) < p_micro; }
};

std::uint64_t to_micro(double p) {
  if (p <= 0) return 0;
  if (p >= 1) return 1000000;
  return static_cast<std::uint64_t>(p * 1e6 + 0.5);
}

// Natural log of u in (0, 1], computed with only + - * / so the result is bit-identical on every
// IEEE-754 platform (std::log is not guaranteed to be). u is given as an integer m in [1, 2^53],
// u = m / 2^53. ln(u) = ln(m') + (e - 53) ln 2 with m' in [0.5, 1) the mantissa of m.
double det_log_unit(std::uint64_t m) {
  constexpr double kLn2 = 0.693147180559945309417232121458;
  int e = 0;
  std::uint64_t t = m;
  while (t) {
    ++e;
    t >>= 1;
  }  // e = bit length of m
  double mant = static_cast<double>(m);
  for (int i = 0; i < e; ++i) mant /= 2.0;  // exact: power-of-two scaling, mant in [0.5, 1)
  const double z = (mant - 1.0) / (mant + 1.0);
  const double z2 = z * z;
  double term = z, sum = 0.0;
  for (int k = 1; k <= 41; k += 2) {  // 2 * atanh(z)
    sum += term / static_cast<double>(k);
    term *= z2;
  }
  return 2.0 * sum + static_cast<double>(e - 53) * kLn2;
}

std::uint64_t exp_gap_ns(Rng& r, double mean_ns) {
  const std::uint64_t m = (r.next() >> 11) + 1;  // 1 .. 2^53
  const double gap = -det_log_unit(m) * mean_ns;
  return static_cast<std::uint64_t>(gap) + 1;  // at least 1 ns so timestamps strictly increase
}

}  // namespace

const char* to_string(DepthDist d) {
  switch (d) {
    case DepthDist::Geometric: return "geometric";
    case DepthDist::Uniform: return "uniform";
    case DepthDist::FatTail: return "fat_tail";
  }
  return "?";
}

bool parse_depth_dist(const std::string& s, DepthDist& out) {
  if (s == "geometric") out = DepthDist::Geometric;
  else if (s == "uniform") out = DepthDist::Uniform;
  else if (s == "fat_tail") out = DepthDist::FatTail;
  else return false;
  return true;
}

// ----------------------------------------------------------------------------- Recorder
Recorder::Recorder(std::size_t snapshot_every, std::size_t depth_levels) {
  run_.snapshot_every = snapshot_every ? snapshot_every : 1;
  run_.depth_levels = depth_levels;
  book_ = std::make_unique<OrderBook>([this](const Event& e) {
    hasher_(e);
    run_.events.push_back({run_.ops.empty() ? 0 : run_.ops.size() - 1, e});
  });
  snapshot();
}

void Recorder::snapshot() {
  Snapshot s;
  s.op_index = run_.ops.size();
  s.t_ns = run_.ops.empty() ? 0 : run_.ops.back().t_ns;
  auto top = [&](Side side, std::vector<LevelInfo>& out) {
    out = book_->levels(side);
    if (out.size() > run_.depth_levels) out.resize(run_.depth_levels);
  };
  top(Side::Buy, s.bids);
  top(Side::Sell, s.asks);
  run_.snapshots.push_back(std::move(s));
}

void Recorder::apply_op(const TimedOp& op) {
  run_.ops.push_back(op);
  apply(*book_, op.op);
  if (run_.ops.size() % run_.snapshot_every == 0) snapshot();
}

Run Recorder::finish(const MarketConfig& cfg) {
  if (run_.snapshots.back().op_index != run_.ops.size()) snapshot();
  run_.config = cfg;
  run_.checksum = digest_book(hasher_.h, *book_);
  return std::move(run_);
}

// ---------------------------------------------------------------------------- simulate
Run simulate(const MarketConfig& cfg, std::size_t snapshot_every, std::size_t depth_levels) {
  Rng rng{cfg.seed};
  Recorder rec(snapshot_every, depth_levels);
  const OrderBook& book = rec.book();

  const std::uint64_t p_cancel = to_micro(cfg.cancel_fraction);
  const std::uint64_t p_modify = to_micro(cfg.modify_fraction);
  const std::uint64_t p_aggr = to_micro(cfg.aggressiveness);
  const std::uint64_t p_drift = to_micro(cfg.drift_prob);
  const double mean_gap_ns = 1e9 / cfg.arrival_rate_per_sec;

  Price fundamental = cfg.initial_mid;
  std::uint64_t t = 0;
  OrderId next_id = 1;
  std::vector<OrderId> open;  // GTC orders that may still rest (validated lazily against the book)

  auto qty = [&]() -> Qty {
    const Qty base = 1 + static_cast<Qty>(rng.below(static_cast<std::uint64_t>(std::max(1, cfg.max_qty))));
    return rng.below(10) == 0 ? base * 5 : base;
  };
  auto depth = [&]() -> Price {
    const std::uint64_t mean = static_cast<std::uint64_t>(std::max(1, cfg.depth_mean_ticks));
    switch (cfg.depth_dist) {
      case DepthDist::Uniform: return static_cast<Price>(rng.below(2 * mean + 1));
      case DepthDist::FatTail: {
        const std::uint64_t m = rng.below(5) == 0 ? mean * 5 : mean;  // 80% near, 20% far
        std::uint64_t k = 0;
        while (k < 20 * m && rng.below(m) != 0) ++k;
        return static_cast<Price>(k);
      }
      case DepthDist::Geometric:
      default: {
        std::uint64_t k = 0;
        while (k < 20 * mean && rng.below(mean) != 0) ++k;
        return static_cast<Price>(k);
      }
    }
  };
  // Picks a live open order (removing dead entries met on the way); returns 0 if none.
  auto pick_live = [&](bool remove) -> OrderId {
    for (int tries = 0; tries < 8 && !open.empty(); ++tries) {
      const std::size_t i = static_cast<std::size_t>(rng.below(open.size()));
      const OrderId id = open[i];
      if (!book.contains(id)) {
        open[i] = open.back();
        open.pop_back();
        continue;
      }
      if (remove) {
        open[i] = open.back();
        open.pop_back();
      }
      return id;
    }
    return 0;
  };

  for (std::uint64_t n = 0; n < cfg.ops; ++n) {
    t += exp_gap_ns(rng, mean_gap_ns);
    if (rng.chance(p_drift)) fundamental += rng.below(2) ? 1 : -1;
    if (fundamental < 100) fundamental = 100;

    Op op;
    bool done = false;
    const std::uint64_t dice = rng.below(1000000);

    if (dice < p_cancel || open.size() > cfg.book_target + cfg.book_target / 2) {
      if (const OrderId id = pick_live(true)) {
        op.kind = Op::Kind::Cancel;
        op.id = id;
        done = true;
      }
    } else if (dice < p_cancel + p_modify) {
      if (const OrderId id = pick_live(false)) {
        const auto view = book.find(id);
        op.kind = Op::Kind::Modify;
        op.id = id;
        if (rng.below(2)) {  // reduce size at the same price (keeps priority)
          op.price = view->price;
          op.qty = view->qty > 1 ? 1 + static_cast<Qty>(rng.below(static_cast<std::uint64_t>(view->qty))) : 1;
        } else {  // move the price a little (loses priority)
          const Price d = static_cast<Price>(rng.below(3)) - 1;
          op.price = std::max<Price>(1, view->price + (d == 0 ? 1 : d));
          op.qty = view->qty;
        }
        done = true;
      }
    }
    if (!done) {
      op = Op{};
      op.kind = Op::Kind::Submit;
      op.id = next_id++;
      op.side = rng.below(2) ? Side::Buy : Side::Sell;
      op.qty = qty();
      if (rng.chance(p_aggr)) {
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        const Price k = static_cast<Price>(rng.below(3));
        const std::uint64_t kind = rng.below(100);
        const bool have_touch = op.side == Side::Buy ? ba.has_value() : bb.has_value();
        if (kind < 30 || !have_touch) {
          op.type = OrderType::Market;
          op.tif = TimeInForce::IOC;
        } else {
          op.type = OrderType::Limit;
          op.price = op.side == Side::Buy ? *ba + k : std::max<Price>(1, *bb - k);
          op.tif = kind < 80 ? TimeInForce::IOC : kind < 90 ? TimeInForce::FOK : TimeInForce::GTC;
        }
      } else {
        op.type = OrderType::Limit;
        op.tif = TimeInForce::GTC;
        const Price d = 1 + depth();
        op.price = op.side == Side::Buy ? std::max<Price>(1, fundamental - d) : fundamental + d;
      }
      if (op.tif == TimeInForce::GTC && op.type == OrderType::Limit) open.push_back(op.id);
    }
    rec.apply_op({t, op});
  }
  return rec.finish(cfg);
}

}  // namespace lob::sim
