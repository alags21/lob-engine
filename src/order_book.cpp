#include "lob/order_book.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace lob {

namespace {

// Does a resting level at `level_price` satisfy a taker on `side` with an optional limit?
bool crosses(Side side, std::optional<Price> limit, Price level_price) {
  if (!limit) return true;
  return side == Side::Buy ? level_price <= *limit : level_price >= *limit;
}

// Runs OrderBook::validate() when the owning public call returns (debug builds only).
struct DebugCheckGuard {
#ifndef NDEBUG
  const OrderBook& book;
  explicit DebugCheckGuard(const OrderBook& b) : book(b) {}
  ~DebugCheckGuard() { book.debug_check_public(); }
#else
  explicit DebugCheckGuard(const OrderBook&) {}
#endif
};

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
  DebugCheckGuard guard(*this);
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
  DebugCheckGuard guard(*this);
  Node* n = index_.find(id);
  if (!n) {
    emit(make_reject(id, RejectReason::UnknownOrder));
    return RejectReason::UnknownOrder;
  }
  Event c;
  c.type = EventType::Cancelled;
  c.order_id = id;
  c.side = n->side;
  c.price = n->price;
  c.qty = n->qty;
  c.cancel = CancelReason::UserRequest;
  unlink(n);
  index_.erase(id);
  pool_.release(n);
  emit(c);
  return RejectReason::None;
}

RejectReason OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
  DebugCheckGuard guard(*this);
  Node* n = index_.find(id);
  if (!n) {
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

  const Side side = n->side;
  const Price old_price = n->price;
  const Qty old_qty = n->qty;
  Event m;
  m.type = EventType::Modified;
  m.order_id = id;
  m.side = side;
  m.price = new_price;
  m.qty = new_qty;

  if (new_price == old_price && new_qty <= old_qty) {
    // Same price, no size increase: in place, priority kept.
    if (new_qty < old_qty) {
      Level& lvl = side == Side::Buy ? bids_.at(old_price) : asks_.at(old_price);
      lvl.total -= old_qty - new_qty;
      n->qty = new_qty;
    }
    m.lost_priority = false;
    emit(m);
    return RejectReason::None;
  }

  // Cancel/replace: priority lost; may now cross and trade as a taker.
  unlink(n);
  index_.erase(id);
  pool_.release(n);
  m.lost_priority = true;
  emit(m);
  const Qty left = match(id, side, new_price, new_qty);
  if (left > 0) rest(id, side, new_price, left);
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
    while (qty > 0 && lvl.head) {
      Node* maker = lvl.head;
      const Qty fill = std::min(qty, maker->qty);
      Event t;
      t.type = EventType::Trade;
      t.order_id = id;
      t.maker_id = maker->id;
      t.side = side;
      t.price = lvl_it->first;
      t.qty = fill;
      qty -= fill;
      maker->qty -= fill;
      lvl.total -= fill;
      if (maker->qty == 0) {
        index_.erase(maker->id);
        lvl.head = maker->next;
        if (lvl.head) lvl.head->prev = nullptr;
        else lvl.tail = nullptr;
        --lvl.count;
        pool_.release(maker);
      }
      emit(t);
    }
    if (!lvl.head) opp.erase(lvl_it);
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
    Node* n = pool_.acquire(id, qty, price, side, lvl.tail);
    if (lvl.tail) lvl.tail->next = n;
    else lvl.head = n;
    lvl.tail = n;
    lvl.total += qty;
    ++lvl.count;
    index_.insert(id, n);
  };
  if (side == Side::Buy) {
    place(bids_);
  } else {
    place(asks_);
  }
}

void OrderBook::unlink(Node* n) {
  auto remove = [&](auto& book) {
    auto lvl_it = book.find(n->price);
    Level& lvl = lvl_it->second;
    lvl.total -= n->qty;
    --lvl.count;
    if (n->prev) n->prev->next = n->next;
    else lvl.head = n->next;
    if (n->next) n->next->prev = n->prev;
    else lvl.tail = n->prev;
    if (!lvl.head) book.erase(lvl_it);
  };
  if (n->side == Side::Buy) {
    remove(bids_);
  } else {
    remove(asks_);
  }
}

void OrderBook::debug_check_public() const {
  const std::string why = validate();
  if (!why.empty()) {
    std::fprintf(stderr, "lob: invariant violated: %s\n", why.c_str());
    std::abort();
  }
}

std::string OrderBook::validate() const {
  std::size_t orders = 0;
  auto check_side = [&](const auto& book, Side side) -> std::string {
    for (const auto& [price, lvl] : book) {
      if (price <= 0) return "non-positive level price";
      if (!lvl.head || !lvl.tail) return "empty level present at " + std::to_string(price);
      if (lvl.head->prev || lvl.tail->next) return "list ends not terminated at " + std::to_string(price);
      Qty sum = 0;
      std::size_t cnt = 0;
      const Node* prev = nullptr;
      for (const Node* n = lvl.head; n; prev = n, n = n->next) {
        if (n->prev != prev) return "broken prev link, order " + std::to_string(n->id);
        if (n->qty <= 0) return "non-positive resting qty, order " + std::to_string(n->id);
        if (n->side != side || n->price != price) return "node side/price mismatch, order " + std::to_string(n->id);
        sum += n->qty;
        ++cnt;
        if (index_.find(n->id) != n) return "index missing or wrong pointer for order " + std::to_string(n->id);
      }
      if (prev != lvl.tail) return "tail pointer wrong at " + std::to_string(price);
      if (sum != lvl.total) return "level total mismatch at " + std::to_string(price);
      if (cnt != lvl.count) return "level count mismatch at " + std::to_string(price);
      orders += cnt;
    }
    return {};
  };
  if (auto e = check_side(bids_, Side::Buy); !e.empty()) return e;
  if (auto e = check_side(asks_, Side::Sell); !e.empty()) return e;
  if (orders != index_.size()) return "index size differs from resting order count";
  if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
    return "crossed or locked book";
  return {};
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
  const Node* n = index_.find(id);
  if (!n) return std::nullopt;
  return OrderView{n->side, n->price, n->qty};
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
    if (it != book.end())
      for (const Node* n = it->second.head; n; n = n->next) out.push_back({n->id, n->qty});
    return out;
  };
  return side == Side::Buy ? collect(bids_) : collect(asks_);
}

std::vector<LevelInfo> OrderBook::levels(Side side) const {
  auto collect = [](const auto& book) {
    std::vector<LevelInfo> out;
    for (const auto& [price, lvl] : book) out.push_back({price, lvl.total, lvl.count});
    return out;
  };
  return side == Side::Buy ? collect(bids_) : collect(asks_);
}

}  // namespace lob
