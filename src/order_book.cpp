#include "lob/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace lob {

namespace {

// Does a resting level at `level_price` satisfy a taker on `side` with an optional limit?
bool crosses(Side side, std::optional<Price> limit, Price level_price) {
  if (!limit) return true;
  return side == Side::Buy ? level_price <= *limit : level_price >= *limit;
}

Event make_reject(OrderId id, RejectReason r) {
  Event e;
  e.type = EventType::Rejected;
  e.order_id = id;
  e.reject = r;
  return e;
}

}  // namespace

OrderBook::OrderBook(EventHandler handler) : handler_(std::move(handler)) {}

RejectReason OrderBook::submit(const NewOrder& o) {
  auto reject = [&](RejectReason r) {
    emit(make_reject(o.id, r));
    return r;
  };
  if (o.qty <= 0) return reject(RejectReason::InvalidQuantity);
  if (o.type == OrderType::Limit && o.price <= 0) return reject(RejectReason::InvalidPrice);
  if (index_.contains(o.id)) return reject(RejectReason::DuplicateId);

  const bool is_market = o.type == OrderType::Market;
  const std::optional<Price> limit = is_market ? std::nullopt : std::optional<Price>(o.price);

  Event acc;
  acc.type = EventType::Accepted;
  acc.order_id = o.id;
  acc.side = o.side;
  acc.price = is_market ? 0 : o.price;
  acc.qty = o.qty;
  emit(acc);

  auto cancel_event = [&](Qty q, CancelReason why) {
    Event c;
    c.type = EventType::Cancelled;
    c.order_id = o.id;
    c.side = o.side;
    c.qty = q;
    c.cancel = why;
    emit(c);
  };

  if (o.tif == TimeInForce::FOK && available_for(o.side, limit, o.qty) < o.qty) {
    cancel_event(o.qty, CancelReason::FokUnfillable);
    return RejectReason::None;
  }

  const Qty left = match(o.id, o.side, limit, o.qty);
  if (left == 0) return RejectReason::None;

  if (is_market) {
    cancel_event(left, CancelReason::MarketRemainder);
  } else if (o.tif == TimeInForce::GTC) {
    rest(o.id, o.side, o.price, left);
  } else {
    // IOC (FOK cannot reach here with a remainder: it was pre-checked).
    cancel_event(left, CancelReason::IocRemainder);
  }
  return RejectReason::None;
}

RejectReason OrderBook::cancel(OrderId id) {
  auto it = index_.find(id);
  if (it == index_.end()) {
    emit(make_reject(id, RejectReason::UnknownOrder));
    return RejectReason::UnknownOrder;
  }
  const Locator loc = it->second;
  Event c;
  c.type = EventType::Cancelled;
  c.order_id = id;
  c.side = loc.side;
  c.price = loc.price;
  c.qty = loc.it->qty;
  c.cancel = CancelReason::UserRequest;
  erase_resting(loc);
  index_.erase(it);
  emit(c);
  return RejectReason::None;
}

RejectReason OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
  auto it = index_.find(id);
  if (it == index_.end()) {
    emit(make_reject(id, RejectReason::UnknownOrder));
    return RejectReason::UnknownOrder;
  }
  if (new_qty <= 0) {
    emit(make_reject(id, RejectReason::InvalidQuantity));
    return RejectReason::InvalidQuantity;
  }
  if (new_price <= 0) {
    emit(make_reject(id, RejectReason::InvalidPrice));
    return RejectReason::InvalidPrice;
  }

  const Locator loc = it->second;
  const Qty old_qty = loc.it->qty;
  Event m;
  m.type = EventType::Modified;
  m.order_id = id;
  m.side = loc.side;
  m.price = new_price;
  m.qty = new_qty;

  if (new_price == loc.price && new_qty <= old_qty) {
    // Same price, no size increase: in place, priority kept.
    if (new_qty < old_qty) {
      Level& lvl = loc.side == Side::Buy ? bids_.at(loc.price) : asks_.at(loc.price);
      lvl.total -= old_qty - new_qty;
      loc.it->qty = new_qty;
    }
    m.lost_priority = false;
    emit(m);
    return RejectReason::None;
  }

  // Cancel/replace: priority lost; may now cross and trade as a taker.
  erase_resting(loc);
  index_.erase(it);
  m.lost_priority = true;
  emit(m);
  const Qty left = match(id, loc.side, new_price, new_qty);
  if (left > 0) rest(id, loc.side, new_price, left);
  return RejectReason::None;
}

Qty OrderBook::match(OrderId id, Side side, std::optional<Price> limit, Qty qty) {
  return side == Side::Buy ? match_against(asks_, id, side, limit, qty)
                           : match_against(bids_, id, side, limit, qty);
}

template <class Book>
Qty OrderBook::match_against(Book& opp, OrderId id, Side side, std::optional<Price> limit,
                             Qty qty) {
  while (qty > 0 && !opp.empty()) {
    auto lvl_it = opp.begin();
    if (!crosses(side, limit, lvl_it->first)) break;
    Level& lvl = lvl_it->second;
    while (qty > 0 && !lvl.queue.empty()) {
      RestingOrder& maker = lvl.queue.front();
      const Qty fill = std::min(qty, maker.qty);
      Event t;
      t.type = EventType::Trade;
      t.order_id = id;
      t.maker_id = maker.id;
      t.side = side;
      t.price = lvl_it->first;
      t.qty = fill;
      qty -= fill;
      maker.qty -= fill;
      lvl.total -= fill;
      if (maker.qty == 0) {
        index_.erase(maker.id);
        lvl.queue.pop_front();
      }
      emit(t);
    }
    if (lvl.queue.empty()) opp.erase(lvl_it);
  }
  return qty;
}

template <class Book>
Qty OrderBook::available(const Book& opp, Side side, std::optional<Price> limit, Qty need) const {
  Qty total = 0;
  for (const auto& [price, lvl] : opp) {
    if (!crosses(side, limit, price)) break;
    total += lvl.total;
    if (total >= need) break;
  }
  return total;
}

Qty OrderBook::available_for(Side side, std::optional<Price> limit, Qty need) const {
  return side == Side::Buy ? available(asks_, side, limit, need)
                           : available(bids_, side, limit, need);
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
  auto place = [&](auto& book) {
    Level& lvl = book[price];
    lvl.queue.push_back({id, qty});
    lvl.total += qty;
    index_.emplace(id, Locator{side, price, std::prev(lvl.queue.end())});
  };
  if (side == Side::Buy) {
    place(bids_);
  } else {
    place(asks_);
  }
}

// Removes the order from its level (and the level if it becomes empty). Caller erases the index.
void OrderBook::erase_resting(const Locator& loc) {
  auto remove = [&](auto& book) {
    auto lvl_it = book.find(loc.price);
    Level& lvl = lvl_it->second;
    lvl.total -= loc.it->qty;
    lvl.queue.erase(loc.it);
    if (lvl.queue.empty()) book.erase(lvl_it);
  };
  if (loc.side == Side::Buy) {
    remove(bids_);
  } else {
    remove(asks_);
  }
}

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::optional<OrderBook::OrderView> OrderBook::find(OrderId id) const {
  auto it = index_.find(id);
  if (it == index_.end()) return std::nullopt;
  return OrderView{it->second.side, it->second.price, it->second.it->qty};
}

Qty OrderBook::qty_at(Side side, Price price) const {
  if (side == Side::Buy) {
    auto it = bids_.find(price);
    return it == bids_.end() ? 0 : it->second.total;
  }
  auto it = asks_.find(price);
  return it == asks_.end() ? 0 : it->second.total;
}

std::vector<RestingOrder> OrderBook::orders_at(Side side, Price price) const {
  auto collect = [&](const auto& book) {
    std::vector<RestingOrder> out;
    auto it = book.find(price);
    if (it != book.end()) out.assign(it->second.queue.begin(), it->second.queue.end());
    return out;
  };
  return side == Side::Buy ? collect(bids_) : collect(asks_);
}

std::vector<LevelInfo> OrderBook::levels(Side side) const {
  auto collect = [](const auto& book) {
    std::vector<LevelInfo> out;
    for (const auto& [price, lvl] : book) out.push_back({price, lvl.total, lvl.queue.size()});
    return out;
  };
  return side == Side::Buy ? collect(bids_) : collect(asks_);
}

}  // namespace lob
