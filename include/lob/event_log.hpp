#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lob/types.hpp"

namespace lob {

/// One input operation to a book. Used for the deterministic input log (docs/EVENT_FORMAT.md).
struct Op {
  enum class Kind : std::uint8_t { Submit, Cancel, Modify };
  Kind kind = Kind::Submit;
  OrderId id = 0;
  Side side = Side::Buy;                  // Submit only
  OrderType type = OrderType::Limit;      // Submit only
  TimeInForce tif = TimeInForce::GTC;     // Submit only
  Price price = 0;                        // Submit, Modify
  Qty qty = 0;                            // Submit, Modify

  friend bool operator==(const Op&, const Op&) = default;
};

/// Text encoding of an input op, one line, no trailing newline. See docs/EVENT_FORMAT.md.
std::string to_string(const Op& op);
/// Inverse of to_string(Op). Returns nullopt for blank lines, comments ('#') and malformed input.
std::optional<Op> parse_op(std::string_view line);

/// Text encoding of an output event, one line, no trailing newline.
std::string to_string(const Event& e);
/// Inverse of to_string(Event); nullopt on malformed input.
std::optional<Event> parse_event(std::string_view line);

/// Apply an op to anything with submit/cancel/modify (OrderBook, ReferenceBook).
template <class Book>
RejectReason apply(Book& book, const Op& op) {
  switch (op.kind) {
    case Op::Kind::Submit:
      return book.submit({op.id, op.side, op.type, op.tif, op.price, op.qty});
    case Op::Kind::Cancel:
      return book.cancel(op.id);
    case Op::Kind::Modify:
      return book.modify(op.id, op.price, op.qty);
  }
  return RejectReason::None;
}

}  // namespace lob
