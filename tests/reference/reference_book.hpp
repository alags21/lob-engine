#pragma once

// Slow, deliberately simple reference matching engine used as the oracle for differential
// fuzzing. It implements the semantics in docs/DESIGN.md independently of lob::OrderBook:
// std::map<price, std::deque<order>> per side, no id index (cancel/modify scan every level),
// no cached totals. Do not optimise this file; its value is that it is obviously correct.

#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "lob/types.hpp"

namespace lob::ref {

class ReferenceBook {
 public:
  using EventHandler = std::function<void(const Event&)>;
  explicit ReferenceBook(EventHandler h = {}) : handler_(std::move(h)) {}

  RejectReason submit(const NewOrder& o);
  RejectReason cancel(OrderId id);
  RejectReason modify(OrderId id, Price new_price, Qty new_qty);

  // Snapshot in the same shape as OrderBook's inspection API.
  std::vector<LevelInfo> levels(Side side) const;
  std::vector<RestingOrder> orders_at(Side side, Price price) const;
  std::size_t order_count() const;

 private:
  struct Order {
    OrderId id;
    Qty qty;
  };
  using Queue = std::deque<Order>;

  bool is_live(OrderId id) const;
  void emit(const Event& e) const {
    if (handler_) handler_(e);
  }
  // Returns the unfilled remainder.
  Qty run_match(OrderId id, Side side, bool is_market, Price limit, Qty qty);
  Qty crossing_liquidity(Side side, bool is_market, Price limit) const;
  void add_resting(Side side, Price price, OrderId id, Qty qty);

  std::map<Price, Queue> bids_;  // best = highest key
  std::map<Price, Queue> asks_;  // best = lowest key
  EventHandler handler_;
};

}  // namespace lob::ref
