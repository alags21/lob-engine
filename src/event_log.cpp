#include "lob/event_log.hpp"

#include <charconv>
#include <sstream>
#include <vector>

namespace lob {

namespace {

std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
    std::size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

template <class T>
bool to_int(std::string_view s, T& out) {
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && p == s.data() + s.size();
}

char side_char(Side s) { return s == Side::Buy ? 'B' : 'S'; }
bool parse_side(std::string_view s, Side& out) {
  if (s == "B") out = Side::Buy;
  else if (s == "S") out = Side::Sell;
  else return false;
  return true;
}

const char* tif_name(TimeInForce t) {
  switch (t) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::FOK: return "FOK";
  }
  return "?";
}
bool parse_tif(std::string_view s, TimeInForce& out) {
  if (s == "GTC") out = TimeInForce::GTC;
  else if (s == "IOC") out = TimeInForce::IOC;
  else if (s == "FOK") out = TimeInForce::FOK;
  else return false;
  return true;
}

const char* reject_name(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "NONE";
    case RejectReason::DuplicateId: return "DUPLICATE_ID";
    case RejectReason::InvalidQuantity: return "INVALID_QTY";
    case RejectReason::InvalidPrice: return "INVALID_PRICE";
    case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
  }
  return "?";
}
bool parse_reject(std::string_view s, RejectReason& out) {
  for (auto r : {RejectReason::None, RejectReason::DuplicateId, RejectReason::InvalidQuantity,
                 RejectReason::InvalidPrice, RejectReason::UnknownOrder}) {
    if (s == reject_name(r)) {
      out = r;
      return true;
    }
  }
  return false;
}

const char* cancel_name(CancelReason r) {
  switch (r) {
    case CancelReason::UserRequest: return "USER";
    case CancelReason::IocRemainder: return "IOC";
    case CancelReason::MarketRemainder: return "MARKET";
    case CancelReason::FokUnfillable: return "FOK";
  }
  return "?";
}
bool parse_cancel(std::string_view s, CancelReason& out) {
  for (auto r : {CancelReason::UserRequest, CancelReason::IocRemainder,
                 CancelReason::MarketRemainder, CancelReason::FokUnfillable}) {
    if (s == cancel_name(r)) {
      out = r;
      return true;
    }
  }
  return false;
}

}  // namespace

std::string to_string(const Op& op) {
  std::ostringstream o;
  switch (op.kind) {
    case Op::Kind::Submit:
      o << "S " << op.id << ' ' << side_char(op.side) << ' '
        << (op.type == OrderType::Limit ? 'L' : 'M') << ' ' << tif_name(op.tif) << ' ' << op.price
        << ' ' << op.qty;
      break;
    case Op::Kind::Cancel:
      o << "C " << op.id;
      break;
    case Op::Kind::Modify:
      o << "M " << op.id << ' ' << op.price << ' ' << op.qty;
      break;
  }
  return o.str();
}

std::optional<Op> parse_op(std::string_view line) {
  auto t = split(line);
  if (t.empty() || t[0][0] == '#') return std::nullopt;
  Op op;
  if (t[0] == "S" && t.size() == 7) {
    op.kind = Op::Kind::Submit;
    if (!to_int(t[1], op.id) || !parse_side(t[2], op.side) || !parse_tif(t[4], op.tif) ||
        !to_int(t[5], op.price) || !to_int(t[6], op.qty))
      return std::nullopt;
    if (t[3] == "L") op.type = OrderType::Limit;
    else if (t[3] == "M") op.type = OrderType::Market;
    else return std::nullopt;
    return op;
  }
  if (t[0] == "C" && t.size() == 2) {
    op.kind = Op::Kind::Cancel;
    if (!to_int(t[1], op.id)) return std::nullopt;
    return op;
  }
  if (t[0] == "M" && t.size() == 4) {
    op.kind = Op::Kind::Modify;
    if (!to_int(t[1], op.id) || !to_int(t[2], op.price) || !to_int(t[3], op.qty))
      return std::nullopt;
    return op;
  }
  return std::nullopt;
}

std::string to_string(const Event& e) {
  std::ostringstream o;
  switch (e.type) {
    case EventType::Accepted:
      o << "A " << e.order_id << ' ' << side_char(e.side) << ' ' << e.price << ' ' << e.qty;
      break;
    case EventType::Rejected:
      o << "R " << e.order_id << ' ' << reject_name(e.reject);
      break;
    case EventType::Trade:
      o << "T " << e.order_id << ' ' << e.maker_id << ' ' << side_char(e.side) << ' ' << e.price
        << ' ' << e.qty;
      break;
    case EventType::Cancelled:
      o << "X " << e.order_id << ' ' << side_char(e.side) << ' ' << e.price << ' ' << e.qty << ' '
        << cancel_name(e.cancel);
      break;
    case EventType::Modified:
      o << "D " << e.order_id << ' ' << side_char(e.side) << ' ' << e.price << ' ' << e.qty << ' '
        << (e.lost_priority ? 1 : 0);
      break;
  }
  return o.str();
}

std::optional<Event> parse_event(std::string_view line) {
  auto t = split(line);
  if (t.empty()) return std::nullopt;
  Event e;
  if (t[0] == "A" && t.size() == 5) {
    e.type = EventType::Accepted;
    if (!to_int(t[1], e.order_id) || !parse_side(t[2], e.side) || !to_int(t[3], e.price) ||
        !to_int(t[4], e.qty))
      return std::nullopt;
    return e;
  }
  if (t[0] == "R" && t.size() == 3) {
    e.type = EventType::Rejected;
    if (!to_int(t[1], e.order_id) || !parse_reject(t[2], e.reject)) return std::nullopt;
    return e;
  }
  if (t[0] == "T" && t.size() == 6) {
    e.type = EventType::Trade;
    if (!to_int(t[1], e.order_id) || !to_int(t[2], e.maker_id) || !parse_side(t[3], e.side) ||
        !to_int(t[4], e.price) || !to_int(t[5], e.qty))
      return std::nullopt;
    return e;
  }
  if (t[0] == "X" && t.size() == 6) {
    e.type = EventType::Cancelled;
    if (!to_int(t[1], e.order_id) || !parse_side(t[2], e.side) || !to_int(t[3], e.price) ||
        !to_int(t[4], e.qty) || !parse_cancel(t[5], e.cancel))
      return std::nullopt;
    return e;
  }
  if (t[0] == "D" && t.size() == 6) {
    e.type = EventType::Modified;
    int lost = 0;
    if (!to_int(t[1], e.order_id) || !parse_side(t[2], e.side) || !to_int(t[3], e.price) ||
        !to_int(t[4], e.qty) || !to_int(t[5], lost) || (lost != 0 && lost != 1))
      return std::nullopt;
    e.lost_priority = lost == 1;
    return e;
  }
  return std::nullopt;
}

}  // namespace lob
