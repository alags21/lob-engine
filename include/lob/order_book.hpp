#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

/// Single-threaded limit order book with price-time priority matching.
///
/// Semantics (see docs/DESIGN.md for the rationale):
///  - Trades execute at the resting (maker) order's price.
///  - Orders at the same price fill in arrival order (FIFO).
///  - There is no self-trade prevention: the book has no notion of participant, so an order
///    that crosses an earlier order with a different id trades with it.
///  - Every call returns RejectReason::None on success; failures also emit a Rejected event.
///  - Events are delivered synchronously, in occurrence order, through the handler.
class OrderBook {
 public:
  using EventHandler = std::function<void(const Event&)>;

  explicit OrderBook(EventHandler handler = {});

  /// Enter a new order. Emits Accepted, then any Trades, then Cancelled for an unfilled
  /// IOC/market remainder or an unfillable FOK.
  RejectReason submit(const NewOrder& order);

  /// Remove a live order. Emits Cancelled(UserRequest) with the remaining quantity.
  /// Cancelling a filled, cancelled or unknown id is rejected with UnknownOrder.
  RejectReason cancel(OrderId id);

  /// Change a live order's price and remaining quantity (`new_qty` is the new *remaining*
  /// quantity, not a delta). A reduction in size at the same price keeps queue priority and is
  /// done in place. Any price change or size increase is cancel/replace: the order goes to the
  /// back of the queue at its new price and, if it now crosses, trades as a taker. Same price
  /// and same size is a no-op that keeps priority. new_qty and new_price must be > 0.
  /// Emits Modified, followed by any Trades.
  RejectReason modify(OrderId id, Price new_price, Qty new_qty);

  // ---- Inspection (does not change the book) ----
  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  bool contains(OrderId id) const { return index_.contains(id); }

  struct OrderView {
    Side side;
    Price price;
    Qty qty;
  };
  /// Resting state of a live order.
  std::optional<OrderView> find(OrderId id) const;
  /// Total resting quantity at a price (0 if no level).
  Qty qty_at(Side side, Price price) const;
  /// Resting orders at a level in time priority (front fills first).
  std::vector<RestingOrder> orders_at(Side side, Price price) const;
  /// Levels from best to worst.
  std::vector<LevelInfo> levels(Side side) const;
  std::size_t order_count() const { return index_.size(); }
  bool empty() const { return index_.empty(); }

 private:
  struct Level {
    std::list<RestingOrder> queue;
    Qty total = 0;
  };
  using BidMap = std::map<Price, Level, std::greater<Price>>;
  using AskMap = std::map<Price, Level>;
  struct Locator {
    Side side;
    Price price;
    std::list<RestingOrder>::iterator it;
  };

  void emit(const Event& e) const {
    if (handler_) handler_(e);
  }

  /// Matches a taker against the opposite side; returns the unfilled quantity.
  Qty match(OrderId id, Side side, std::optional<Price> limit, Qty qty);
  template <class Book>
  Qty match_against(Book& opp, OrderId id, Side side, std::optional<Price> limit, Qty qty);
  template <class Book>
  Qty available(const Book& opp, Side side, std::optional<Price> limit, Qty need) const;
  Qty available_for(Side side, std::optional<Price> limit, Qty need) const;
  void rest(OrderId id, Side side, Price price, Qty qty);
  void erase_resting(const Locator& loc);

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Locator> index_;
  EventHandler handler_;
};

}  // namespace lob
