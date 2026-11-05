#include "reference_book.hpp"

#include <algorithm>
#include <iterator>

namespace lob::ref {

namespace {
Event reject_event(OrderId id, RejectReason r) {
  Event e;
  e.type = EventType::Rejected;
  e.order_id = id;
  e.reject = r;
  return e;
}
}  // namespace

bool ReferenceBook::is_live(OrderId id) const {
  for (const auto* side : {&bids_, &asks_})
    for (const auto& [p, q] : *side)
      for (const auto& o : q)
        if (o.id == id) return true;
  return false;
}

std::size_t ReferenceBook::order_count() const {
  std::size_t n = 0;
  for (const auto* side : {&bids_, &asks_})
    for (const auto& [p, q] : *side) n += q.size();
  return n;
}

void ReferenceBook::add_resting(Side side, Price price, OrderId id, Qty qty) {
  (side == Side::Buy ? bids_ : asks_)[price].push_back({id, qty});
}

// Quantity a taker could execute right now at prices acceptable to it.
Qty ReferenceBook::crossing_liquidity(Side side, bool is_market, Price limit) const {
  Qty total = 0;
  if (side == Side::Buy) {
    for (const auto& [p, q] : asks_)
      if (is_market || p <= limit)
        for (const auto& o : q) total += o.qty;
  } else {
    for (const auto& [p, q] : bids_)
      if (is_market || p >= limit)
        for (const auto& o : q) total += o.qty;
  }
  return total;
}

Qty ReferenceBook::run_match(OrderId id, Side side, bool is_market, Price limit, Qty qty) {
  auto& opp = side == Side::Buy ? asks_ : bids_;
  while (qty > 0 && !opp.empty()) {
    // Best opposite level: lowest ask for a buyer, highest bid for a seller.
    auto lvl = side == Side::Buy ? opp.begin() : std::prev(opp.end());
    const Price p = lvl->first;
    if (!is_market) {
      if (side == Side::Buy && p > limit) break;
      if (side == Side::Sell && p < limit) break;
    }
    Queue& q = lvl->second;
    while (qty > 0 && !q.empty()) {
      Order& maker = q.front();
      const Qty fill = std::min(qty, maker.qty);
      Event t;
      t.type = EventType::Trade;
      t.order_id = id;
      t.maker_id = maker.id;
      t.side = side;
      t.price = p;
      t.qty = fill;
      emit(t);
      qty -= fill;
      maker.qty -= fill;
      if (maker.qty == 0) q.pop_front();
    }
    if (q.empty()) opp.erase(lvl);
  }
  return qty;
}

RejectReason ReferenceBook::submit(const NewOrder& o) {
  RejectReason bad = RejectReason::None;
  if (o.qty <= 0) bad = RejectReason::InvalidQuantity;
  else if (o.type == OrderType::Limit && o.price <= 0) bad = RejectReason::InvalidPrice;
  else if (is_live(o.id)) bad = RejectReason::DuplicateId;
  if (bad != RejectReason::None) {
    emit(reject_event(o.id, bad));
    return bad;
  }

  const bool market = o.type == OrderType::Market;
  Event a;
  a.type = EventType::Accepted;
  a.order_id = o.id;
  a.side = o.side;
  a.price = market ? 0 : o.price;
  a.qty = o.qty;
  emit(a);

  auto emit_cancel = [&](Qty q, CancelReason why) {
    Event c;
    c.type = EventType::Cancelled;
    c.order_id = o.id;
    c.side = o.side;
    c.qty = q;
    c.cancel = why;
    emit(c);
  };

  if (o.tif == TimeInForce::FOK && crossing_liquidity(o.side, market, o.price) < o.qty) {
    emit_cancel(o.qty, CancelReason::FokUnfillable);
    return RejectReason::None;
  }
  const Qty left = run_match(o.id, o.side, market, o.price, o.qty);
  if (left == 0) return RejectReason::None;
  if (market) emit_cancel(left, CancelReason::MarketRemainder);
  else if (o.tif == TimeInForce::GTC) add_resting(o.side, o.price, o.id, left);
  else emit_cancel(left, CancelReason::IocRemainder);
  return RejectReason::None;
}

RejectReason ReferenceBook::cancel(OrderId id) {
  for (Side side : {Side::Buy, Side::Sell}) {
    auto& book = side == Side::Buy ? bids_ : asks_;
    for (auto lvl = book.begin(); lvl != book.end(); ++lvl) {
      Queue& q = lvl->second;
      for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->id != id) continue;
        Event c;
        c.type = EventType::Cancelled;
        c.order_id = id;
        c.side = side;
        c.price = lvl->first;
        c.qty = it->qty;
        c.cancel = CancelReason::UserRequest;
        q.erase(it);
        if (q.empty()) book.erase(lvl);
        emit(c);
        return RejectReason::None;
      }
    }
  }
  emit(reject_event(id, RejectReason::UnknownOrder));
  return RejectReason::UnknownOrder;
}

RejectReason ReferenceBook::modify(OrderId id, Price new_price, Qty new_qty) {
  for (Side side : {Side::Buy, Side::Sell}) {
    auto& book = side == Side::Buy ? bids_ : asks_;
    for (auto lvl = book.begin(); lvl != book.end(); ++lvl) {
      Queue& q = lvl->second;
      for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->id != id) continue;
        if (new_qty <= 0) {
          emit(reject_event(id, RejectReason::InvalidQuantity));
          return RejectReason::InvalidQuantity;
        }
        if (new_price <= 0) {
          emit(reject_event(id, RejectReason::InvalidPrice));
          return RejectReason::InvalidPrice;
        }
        Event m;
        m.type = EventType::Modified;
        m.order_id = id;
        m.side = side;
        m.price = new_price;
        m.qty = new_qty;
        if (new_price == lvl->first && new_qty <= it->qty) {
          it->qty = new_qty;  // in place, keeps queue position
          m.lost_priority = false;
          emit(m);
          return RejectReason::None;
        }
        // Cancel/replace.
        q.erase(it);
        if (q.empty()) book.erase(lvl);
        m.lost_priority = true;
        emit(m);
        const Qty left = run_match(id, side, false, new_price, new_qty);
        if (left > 0) add_resting(side, new_price, id, left);
        return RejectReason::None;
      }
    }
  }
  emit(reject_event(id, RejectReason::UnknownOrder));
  return RejectReason::UnknownOrder;
}

std::vector<LevelInfo> ReferenceBook::levels(Side side) const {
  std::vector<LevelInfo> out;
  auto add = [&](Price p, const Queue& q) {
    Qty t = 0;
    for (const auto& o : q) t += o.qty;
    out.push_back({p, t, q.size()});
  };
  if (side == Side::Buy) {
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) add(it->first, it->second);
  } else {
    for (const auto& [p, q] : asks_) add(p, q);
  }
  return out;
}

std::vector<RestingOrder> ReferenceBook::orders_at(Side side, Price price) const {
  const auto& book = side == Side::Buy ? bids_ : asks_;
  auto it = book.find(price);
  std::vector<RestingOrder> out;
  if (it != book.end())
    for (const auto& o : it->second) out.push_back({o.id, o.qty});
  return out;
}

}  // namespace lob::ref
