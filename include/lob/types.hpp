#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

/// Prices are integer ticks; quantities are integer lots. No floating point anywhere.
using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class OrderType : std::uint8_t {
  Limit,   ///< Executes at `price` or better; remainder rests (GTC) or is cancelled (IOC/FOK).
  Market,  ///< Executes at any price; `price` is ignored; remainder is cancelled, never rests.
};

enum class TimeInForce : std::uint8_t {
  GTC,  ///< Good till cancel: unfilled limit remainder rests in the book.
  IOC,  ///< Immediate or cancel: fill what is possible now, cancel the rest.
  FOK,  ///< Fill or kill: fill entirely now or do nothing (no partial trades).
};

/// A request to enter a new order.
struct NewOrder {
  OrderId id = 0;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::GTC;
  Price price = 0;  ///< Ticks, must be > 0 for limit orders. Ignored for market orders.
  Qty qty = 0;      ///< Lots, must be > 0.
};

enum class RejectReason : std::uint8_t {
  None,
  DuplicateId,      ///< An order with this id is already live in the book.
  InvalidQuantity,  ///< qty <= 0.
  InvalidPrice,     ///< Price <= 0 (limit orders and modify).
  UnknownOrder,     ///< Cancel/modify of an id that is not live (never seen, filled, cancelled).
};

enum class CancelReason : std::uint8_t {
  UserRequest,     ///< Explicit cancel().
  IocRemainder,    ///< Unfilled remainder of an IOC order.
  MarketRemainder, ///< Unfilled remainder of a market order (opposite side exhausted).
  FokUnfillable,   ///< FOK order could not be filled entirely; nothing traded.
};

enum class EventType : std::uint8_t { Accepted, Rejected, Trade, Cancelled, Modified };

/// One event emitted by the book. Which fields are meaningful depends on `type`:
///  - Accepted:  order_id, side, price (0 for market), qty (as submitted).
///  - Rejected:  order_id, reject.
///  - Trade:     order_id = taker id, maker_id, side = taker side, price = maker's resting
///               price, qty = executed quantity.
///  - Cancelled: order_id, side, qty = quantity removed, cancel (price = resting price for
///               UserRequest, else 0).
///  - Modified:  order_id, side, price/qty = new values, lost_priority.
struct Event {
  EventType type = EventType::Accepted;
  OrderId order_id = 0;
  OrderId maker_id = 0;
  Side side = Side::Buy;
  Price price = 0;
  Qty qty = 0;
  RejectReason reject = RejectReason::None;
  CancelReason cancel = CancelReason::UserRequest;
  bool lost_priority = false;

  friend bool operator==(const Event&, const Event&) = default;
};

/// An order resting in the book.
struct RestingOrder {
  OrderId id = 0;
  Qty qty = 0;  ///< Remaining quantity.

  friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

/// Aggregate view of one price level.
struct LevelInfo {
  Price price = 0;
  Qty qty = 0;
  std::size_t order_count = 0;

  friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

}  // namespace lob
