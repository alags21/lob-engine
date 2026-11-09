#include "fuzz.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "../reference/reference_book.hpp"
#include "lob/order_book.hpp"

namespace lob::fuzz {

// ---------------------------------------------------------------- generator

// splitmix64: tiny, fast, fully specified, so streams are identical on every platform.
std::uint64_t OpGenerator::rnd() {
  std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

OpGenerator::OpGenerator(std::uint64_t seed) : state_(seed) { known_.reserve(256); }

Op OpGenerator::next() {
  ++count_;
  if (count_ % 6 == 0) {  // mid price random walk, bounded
    const int step = static_cast<int>(below(3)) - 1;
    mid_ += step;
    if (mid_ < 70) mid_ = 70;
    if (mid_ > 130) mid_ = 130;
  }

  auto remember = [&](OrderId id, Price p) {
    if (known_.size() < 256) {
      known_.push_back({id, p});
    } else {
      known_[ring_pos_] = {id, p};
      ring_pos_ = (ring_pos_ + 1) % known_.size();
    }
  };
  auto pick_known = [&]() -> Known {
    if (known_.empty() || below(100) < 8) return {next_id_ + 1000 + below(50), mid_};  // unknown
    return known_[below(known_.size())];
  };
  auto qty = [&]() -> Qty {
    return below(100) < 90 ? static_cast<Qty>(1 + below(20)) : static_cast<Qty>(1 + below(120));
  };

  // Cancels/modifies mostly target ids the generator believes are open (GTC limits it issued and
  // has not cancelled). A thermostat on the pool size keeps the book at a few hundred orders so
  // the O(n) reference model stays fast; stale and unknown ids are still mixed in.
  auto pick_open = [&](bool remove) -> Known {
    const std::uint64_t pct = open_.size() > 300 ? 97 : 30;
    if (open_.empty() || below(100) >= pct) return pick_known();
    const std::size_t idx = static_cast<std::size_t>(below(open_.size()));
    const Known k = open_[idx];
    if (remove) {
      open_[idx] = open_.back();
      open_.pop_back();
    }
    return k;
  };

  const std::uint64_t r = below(100);
  Op op;

  if (r < 56 || r >= 96) {
    // Submit (the r >= 96 slice is invalid / duplicate-id submits).
    op.kind = Op::Kind::Submit;
    op.side = below(2) ? Side::Buy : Side::Sell;
    op.id = next_id_++;
    const std::int64_t off = static_cast<std::int64_t>(below(13));
    // Buys skew below mid, sells above, with an overlap so orders cross regularly.
    op.price = op.side == Side::Buy ? mid_ - 8 + off : mid_ + 8 - off;
    op.qty = qty();
    if (r < 34) {
      op.type = OrderType::Limit;
      op.tif = TimeInForce::GTC;
      open_.push_back({op.id, op.price});
    } else if (r < 42) {
      op.type = OrderType::Limit;
      op.tif = TimeInForce::IOC;
    } else if (r < 48) {
      op.type = OrderType::Limit;
      op.tif = TimeInForce::FOK;
    } else if (r < 56) {
      op.type = OrderType::Market;
      const auto t = below(3);
      op.tif = t == 0 ? TimeInForce::GTC : t == 1 ? TimeInForce::IOC : TimeInForce::FOK;
      op.price = below(2) ? 0 : op.price;  // must be ignored by the book
    } else {
      op.type = below(4) ? OrderType::Limit : OrderType::Market;
      op.tif = TimeInForce::GTC;
      switch (below(3)) {
        case 0: op.qty = below(2) ? 0 : -static_cast<Qty>(1 + below(5)); break;
        case 1: op.price = below(2) ? 0 : -static_cast<Price>(1 + below(5)); break;
        default: op.id = pick_known().id; break;  // duplicate (or stale) id
      }
    }
    remember(op.id, op.price);
  } else if (r < 84) {
    op.kind = Op::Kind::Cancel;
    op.id = pick_open(true).id;
  } else {
    op.kind = Op::Kind::Modify;
    const Known k = pick_open(false);
    op.id = k.id;
    op.price = below(100) < 45 ? k.price : mid_ - 8 + static_cast<Price>(below(17));
    op.qty = qty();
    if (below(100) < 3) op.qty = 0;
    if (below(100) < 2) op.price = 0;
  }
  return op;
}

// ------------------------------------------------------------------ runner

namespace {

std::string describe(const std::vector<Event>& v) {
  std::ostringstream o;
  for (const auto& e : v) o << "    " << to_string(e) << '\n';
  return o.str();
}

template <class A, class B>
bool books_equal(const A& a, const B& b, std::string& why) {
  for (Side s : {Side::Buy, Side::Sell}) {
    const auto la = a.levels(s);
    const auto lb = b.levels(s);
    if (la != lb) {
      why = std::string("levels differ on ") + (s == Side::Buy ? "bid" : "ask") + " side";
      return false;
    }
    for (const auto& lvl : la) {
      if (a.orders_at(s, lvl.price) != b.orders_at(s, lvl.price)) {
        why = "queue (FIFO order or sizes) differs at price " + std::to_string(lvl.price);
        return false;
      }
    }
  }
  return true;
}

}  // namespace

FuzzResult run_fuzz(const FuzzConfig& cfg) {
  FuzzResult res;
  std::vector<Event> got, want;
  OrderBook book([&](const Event& e) { got.push_back(e); });
  ref::ReferenceBook oracle([&](const Event& e) { want.push_back(e); });
  OpGenerator gen(cfg.seed);
  std::vector<Op> history;

  auto fail = [&](const std::string& what) {
    res.ok = false;
    res.failing_prefix = history;
    std::ostringstream o;
    o << "MISMATCH seed=" << cfg.seed << " op_index=" << res.ops << " op=[" << to_string(history.back())
      << "]\n  " << what << "\n  book events:\n"
      << describe(got) << "  reference events:\n"
      << describe(want) << "  reproduce: lob_fuzz --seed " << cfg.seed << " --ops " << res.ops + 1
      << "\n";
    res.message = o.str();
  };

  for (std::uint64_t i = 0; i < cfg.ops; ++i) {
    const Op op = gen.next();
    history.push_back(op);
    got.clear();
    want.clear();
    const RejectReason rg = apply(book, op);
    const RejectReason rw = apply(oracle, op);
    if (rg != rw) return fail("return status differs"), res;
    if (got != want) return fail("events differ"), res;
    if (i % cfg.state_check_interval == 0 || i + 1 == cfg.ops) {
      if (const std::string v = book.validate(); !v.empty()) return fail("invariant: " + v), res;
      std::string why;
      if (!books_equal(book, oracle, why)) return fail(why), res;
    }

    ++res.ops;
    res.events += got.size();
    for (const auto& e : got) {
      if (e.type == EventType::Trade) ++res.trades;
      if (e.type == EventType::Rejected) ++res.rejects;
    }
    res.max_resting_orders = std::max(res.max_resting_orders, book.order_count());
  }
  return res;
}

bool write_op_log(const std::string& path, const std::vector<Op>& ops, std::uint64_t seed) {
  std::ofstream f(path);
  if (!f) return false;
  f << "# lob op log v1 (generated by lob_fuzz, seed " << seed << ")\n";
  for (const auto& op : ops) f << to_string(op) << '\n';
  return static_cast<bool>(f);
}

}  // namespace lob::fuzz
