#include "itch_validate.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace lob::itch {

Stats& Stats::operator+=(const Stats& o) {
  messages += o.messages; adds += o.adds; deletes += o.deletes; partial_cancels += o.partial_cancels; replaces += o.replaces;
  exec_e += o.exec_e; exec_c += o.exec_c; c_not_printable += o.c_not_printable; c_price_differs += o.c_price_differs;
  trades_p += o.trades_p; crosses_q += o.crosses_q; trading_actions += o.trading_actions; exec_shares += o.exec_shares;
  for (int b = 0; b < kBuckets; ++b) {
    exec_checked[b] += o.exec_checked[b]; exec_ok[b] += o.exec_ok[b]; mismatch_odd_lot[b] += o.mismatch_odd_lot[b];
    crossed_adds[b] += o.crossed_adds[b];
    for (int k = 0; k < kMismatchKinds; ++k) mismatch[b][k] += o.mismatch[b][k];
  }
  events_on_unbooked += o.events_on_unbooked;
  for (int k = 0; k < kViolationKinds; ++k) violation[k] += o.violation[k];
  rebuilds += o.rebuilds; ref_compares += o.ref_compares; ref_level_mismatch += o.ref_level_mismatch; ref_queue_mismatch += o.ref_queue_mismatch;
  return *this;
}

struct Validator::Symbol {
  std::uint16_t locate = 0;
  std::string name;
  std::unique_ptr<OrderBook> book;
  std::unique_ptr<lob::ref::ReferenceBook> ref;
  std::vector<Event> ev;
  Stats stats;
  std::unordered_set<std::uint64_t> live;
  char state = 'T';
  bool opened = false, closed = false, diverged = false;
  std::uint64_t msgs = 0;

  void make_book() {
    book = std::make_unique<OrderBook>([this](const Event& e) { ev.push_back(e); });
  }
};

Validator::Validator(ValidateConfig cfg) : cfg_(std::move(cfg)) { orders_.reserve(1 << 20); }
Validator::~Validator() = default;

Validator::Symbol& Validator::sym(std::uint16_t locate) {
  auto& p = syms_[locate];
  if (!p) {
    p = std::make_unique<Symbol>();
    p->locate = locate;
    p->name = "?" + std::to_string(locate);
    p->make_book();
  }
  return *p;
}

int Validator::bucket_of(const Symbol& s) const {
  if (phase_ == 0) return kPre;
  if (phase_ == 2) return kPost;
  return (s.state == 'T' && s.opened && !s.closed) ? kContinuous : kMarketOther;
}

void Validator::add_order(Symbol& s, std::uint64_t ref, char side, std::int64_t price, std::uint32_t shares) {
  if (orders_.count(ref)) {
    ++s.stats.violation[kDuplicateRef];
    return;
  }
  Order o;
  o.locate = s.locate;
  o.side = side;
  o.price = price;
  o.remaining = o.original = shares;
  o.seq = ++seq_;
  o.booked = !s.diverged;
  if (o.booked) {
    const Side sd = side == 'B' ? Side::Buy : Side::Sell;
    const auto opp = sd == Side::Buy ? s.book->best_ask() : s.book->best_bid();
    const bool crossed = opp && (sd == Side::Buy ? *opp <= price : *opp >= price);
    if (crossed) {
      ++s.stats.crossed_adds[bucket_of(s)];
      o.booked = false;
    } else {
      s.ev.clear();
      if (s.book->submit({ref, sd, OrderType::Limit, TimeInForce::GTC, price, static_cast<Qty>(shares)}) != RejectReason::None) {
        ++s.stats.violation[kBookRejected];
        o.booked = false;
      } else if (s.ref) {
        s.ref->submit({ref, sd, OrderType::Limit, TimeInForce::GTC, price, static_cast<Qty>(shares)});
      }
    }
  }
  orders_.emplace(ref, o);
  s.live.insert(ref);
}

void Validator::remove_order(Symbol& s, std::uint64_t ref, Order&) {
  s.live.erase(ref);
  orders_.erase(ref);
}

void Validator::rebuild(Symbol& s) {
  ++s.stats.rebuilds;
  if (s.stats.rebuilds > cfg_.max_rebuilds_per_symbol) {
    s.diverged = true;
    return;
  }
  std::vector<std::tuple<std::int64_t, std::uint64_t, std::uint64_t>> v;  // price, seq, ref
  for (std::uint64_t r : s.live) {
    const Order& o = orders_.at(r);
    if (o.booked) v.emplace_back(o.price, o.seq, r);
  }
  std::sort(v.begin(), v.end());
  s.make_book();
  for (const auto& [price, seq, r] : v) {
    const Order& o = orders_.at(r);
    s.book->submit({r, o.side == 'B' ? Side::Buy : Side::Sell, OrderType::Limit, TimeInForce::GTC, price, static_cast<Qty>(o.remaining)});
  }
  s.ev.clear();
}

void Validator::compare_reference(Symbol& s) {
  if (!s.ref || s.diverged) return;
  ++s.stats.ref_compares;
  bool level_bad = false, queue_bad = false;
  for (Side side : {Side::Buy, Side::Sell}) {
    if (s.book->levels(side) != s.ref->levels(side)) {
      level_bad = true;
      continue;
    }
    for (const auto& lvl : s.book->levels(side))
      if (s.book->orders_at(side, lvl.price) != s.ref->orders_at(side, lvl.price)) queue_bad = true;
  }
  if (level_bad) ++s.stats.ref_level_mismatch;
  else if (queue_bad) ++s.stats.ref_queue_mismatch;
  if (level_bad || queue_bad) rebuild(s);
}

void Validator::execute(Symbol& s, const Message& m) {
  auto it = orders_.find(m.ref);
  if (it == orders_.end()) {
    ++s.stats.violation[kUnknownRef];
    return;
  }
  Order& o = it->second;
  std::uint64_t executed = m.qty;
  if (executed > o.remaining) {
    ++s.stats.violation[kOverExecute];
    executed = o.remaining;
  }
  if (m.type == 'E') ++s.stats.exec_e;
  else {
    ++s.stats.exec_c;
    if (!m.printable) ++s.stats.c_not_printable;
    if (m.price != o.price) ++s.stats.c_price_differs;
  }
  s.stats.exec_shares += executed;
  const std::uint32_t new_rem = o.remaining - static_cast<std::uint32_t>(executed);

  if (!o.booked || s.diverged) {
    ++s.stats.events_on_unbooked;
  } else if (executed > 0) {
    const int b = bucket_of(s);
    ++s.stats.exec_checked[b];
    const Side resting = o.side == 'B' ? Side::Buy : Side::Sell;
    s.ev.clear();
    s.book->submit({ioc_id_++, opposite(resting), OrderType::Limit, TimeInForce::IOC, o.price, static_cast<Qty>(executed)});
    std::vector<Event> trades;
    for (const auto& e : s.ev)
      if (e.type == EventType::Trade) trades.push_back(e);
    const bool ok = trades.size() == 1 && trades[0].maker_id == m.ref && trades[0].price == o.price &&
                    trades[0].qty == static_cast<Qty>(executed);
    if (ok) {
      ++s.stats.exec_ok[b];
    } else {
      int kind = kOtherDifference;
      if (trades.empty()) kind = kNoFill;
      else if (trades[0].maker_id != m.ref) {
        const bool better = resting == Side::Sell ? trades[0].price < o.price : trades[0].price > o.price;
        kind = better ? kBetterPriceAhead : (trades[0].price == o.price ? kSamePriceOtherOrder : kOtherDifference);
      }
      ++s.stats.mismatch[b][kind];
      const Order* other = trades.empty() ? nullptr : (orders_.count(trades[0].maker_id) ? &orders_.at(trades[0].maker_id) : nullptr);
      if (o.original < 100 || (other && other->original < 100)) ++s.stats.mismatch_odd_lot[b];
      if (examples_.size() < cfg_.max_examples) {
        Example e;
        e.symbol = s.name; e.ts_ns = m.ts_ns; e.ref = m.ref; e.price = o.price; e.executed = executed; e.order_shares_orig = o.original;
        e.bucket = b; e.kind = kind; e.side = o.side;
        if (!trades.empty()) { e.other_ref = trades[0].maker_id; e.other_price = trades[0].price; }
        if (other) e.other_shares_orig = other->original;
        examples_.push_back(e);
      }
    }
    // Exchange truth for the reference mirror and the tracker, then resync our book after a mismatch.
    if (s.ref) {
      if (new_rem == 0) s.ref->cancel(m.ref);
      else s.ref->modify(m.ref, o.price, static_cast<Qty>(new_rem));
    }
    o.remaining = new_rem;
    if (new_rem == 0) remove_order(s, m.ref, o);
    if (!ok) {
      rebuild(s);
    } else if (new_rem > 0) {
      const auto v = s.book->find(m.ref);
      if (!v || v->qty != static_cast<Qty>(new_rem)) ++s.stats.violation[kBookQtyMismatch];
    }
    return;
  }
  // unbooked path: tracker only (the reference mirror never saw this order either)
  o.remaining = new_rem;
  if (new_rem == 0) remove_order(s, m.ref, o);
}

void Validator::on_message(const Message& m) {
  ++by_type_[static_cast<unsigned char>(m.type)];
  if (first_ts_ == 0) first_ts_ = m.ts_ns;
  last_ts_ = m.ts_ns;
  if (m.type == 'S') {
    system_events_.emplace(m.code, m.ts_ns);  // first occurrence of each code
    if (m.code == 'Q') phase_ = 1;
    else if (m.code == 'M') phase_ = 2;
    return;
  }
  Symbol& s = sym(m.locate);
  ++s.stats.messages;
  ++s.msgs;
  switch (m.type) {
    case 'R':
      s.name = m.stock;
      if (cfg_.reference_symbols.count(s.name) && !s.ref) s.ref = std::make_unique<lob::ref::ReferenceBook>();
      break;
    case 'H':
      ++s.stats.trading_actions;
      s.state = m.code;
      break;
    case 'A':
    case 'F':
      ++s.stats.adds;
      add_order(s, m.ref, m.side, m.price, static_cast<std::uint32_t>(m.qty));
      break;
    case 'D': {
      ++s.stats.deletes;
      auto it = orders_.find(m.ref);
      if (it == orders_.end()) {
        ++s.stats.violation[kUnknownRef];
        break;
      }
      Order& o = it->second;
      if (o.booked && !s.diverged) {
        s.ev.clear();
        if (s.book->cancel(m.ref) != RejectReason::None) ++s.stats.violation[kBookRejected];
        if (s.ref) s.ref->cancel(m.ref);
      } else {
        ++s.stats.events_on_unbooked;
      }
      remove_order(s, m.ref, o);
      break;
    }
    case 'X': {
      ++s.stats.partial_cancels;
      auto it = orders_.find(m.ref);
      if (it == orders_.end()) {
        ++s.stats.violation[kUnknownRef];
        break;
      }
      Order& o = it->second;
      std::uint64_t cancelled = m.qty;
      if (cancelled > o.remaining) {
        ++s.stats.violation[kOverCancel];
        cancelled = o.remaining;
      }
      const std::uint32_t new_rem = o.remaining - static_cast<std::uint32_t>(cancelled);
      if (o.booked && !s.diverged) {
        s.ev.clear();
        if (new_rem == 0) {
          if (s.book->cancel(m.ref) != RejectReason::None) ++s.stats.violation[kBookRejected];
          if (s.ref) s.ref->cancel(m.ref);
        } else {
          if (s.book->modify(m.ref, o.price, static_cast<Qty>(new_rem)) != RejectReason::None) ++s.stats.violation[kBookRejected];
          if (s.ref) s.ref->modify(m.ref, o.price, static_cast<Qty>(new_rem));
          const auto v = s.book->find(m.ref);
          if (!v || v->qty != static_cast<Qty>(new_rem)) ++s.stats.violation[kBookQtyMismatch];
        }
      } else {
        ++s.stats.events_on_unbooked;
      }
      o.remaining = new_rem;
      if (new_rem == 0) remove_order(s, m.ref, o);
      break;
    }
    case 'U': {
      ++s.stats.replaces;
      auto it = orders_.find(m.ref);
      if (it == orders_.end()) {
        ++s.stats.violation[kUnknownRef];
        break;
      }
      Order& o = it->second;
      const char side = o.side;
      if (o.booked && !s.diverged) {
        s.ev.clear();
        if (s.book->cancel(m.ref) != RejectReason::None) ++s.stats.violation[kBookRejected];
        if (s.ref) s.ref->cancel(m.ref);
      } else {
        ++s.stats.events_on_unbooked;
      }
      remove_order(s, m.ref, o);
      add_order(s, m.new_ref, side, m.price, static_cast<std::uint32_t>(m.qty));
      break;
    }
    case 'E':
    case 'C':
      execute(s, m);
      break;
    case 'P':
      ++s.stats.trades_p;
      break;
    case 'Q':
      ++s.stats.crosses_q;
      if (m.code == 'O') s.opened = true;
      else if (m.code == 'C') s.closed = true;
      break;
    default: break;
  }
  if (s.ref && s.msgs % cfg_.compare_every == 0) compare_reference(s);
}

void Validator::finish() {
  for (auto& [loc, s] : syms_)
    if (s->ref) compare_reference(*s);
}

Stats Validator::total() const {
  Stats t;
  for (const auto& [loc, s] : syms_) t += s->stats;
  return t;
}

std::vector<Validator::SymbolInfo> Validator::symbols() const {
  std::vector<SymbolInfo> v;
  for (const auto& [loc, s] : syms_) {
    SymbolInfo i;
    i.name = s->name;
    i.stats = s->stats;
    i.reference = static_cast<bool>(s->ref);
    i.opened = s->opened;
    i.closed = s->closed;
    i.diverged = s->diverged;
    i.resting_orders = s->book->order_count();
    v.push_back(std::move(i));
  }
  std::sort(v.begin(), v.end(), [](const SymbolInfo& a, const SymbolInfo& b) { return a.stats.messages > b.stats.messages; });
  return v;
}

std::uint64_t Validator::system_event_ts(char code) const {
  auto it = system_events_.find(code);
  return it == system_events_.end() ? 0 : it->second;
}

namespace {
const char* kBucketNames[kBuckets] = {"pre_market", "continuous", "market_hours_other", "post_close"};
const char* kKindNames[kMismatchKinds] = {"no_fill", "better_priced_order_ahead", "same_price_other_order", "other_difference"};
const char* kViolNames[kViolationKinds] = {"unknown_reference", "duplicate_reference", "execute_exceeds_remaining", "cancel_exceeds_remaining",
                                           "book_size_differs_from_exchange", "book_rejected_operation"};

void stats_json(std::ostringstream& o, const Stats& s) {
  o << "{\"messages\":" << s.messages << ",\"adds\":" << s.adds << ",\"deletes\":" << s.deletes << ",\"partial_cancels\":" << s.partial_cancels
    << ",\"replaces\":" << s.replaces << ",\"executions_E\":" << s.exec_e << ",\"executions_C\":" << s.exec_c
    << ",\"C_not_printable\":" << s.c_not_printable << ",\"C_price_differs_from_order\":" << s.c_price_differs << ",\"trades_P\":" << s.trades_p
    << ",\"crosses_Q\":" << s.crosses_q << ",\"trading_actions_H\":" << s.trading_actions << ",\"executed_shares\":" << s.exec_shares
    << ",\"priority_check\":{";
  for (int b = 0; b < kBuckets; ++b) {
    std::uint64_t mm = 0;
    for (int k = 0; k < kMismatchKinds; ++k) mm += s.mismatch[b][k];
    o << (b ? "," : "") << "\"" << kBucketNames[b] << "\":{\"checked\":" << s.exec_checked[b] << ",\"engine_matched_exchange_choice\":" << s.exec_ok[b]
      << ",\"mismatches\":" << mm << ",\"mismatch_by_kind\":{";
    for (int k = 0; k < kMismatchKinds; ++k) o << (k ? "," : "") << "\"" << kKindNames[k] << "\":" << s.mismatch[b][k];
    o << "},\"mismatches_involving_odd_lot\":" << s.mismatch_odd_lot[b] << ",\"adds_that_would_cross_our_book\":" << s.crossed_adds[b] << "}";
  }
  o << "},\"violations\":{";
  for (int k = 0; k < kViolationKinds; ++k) o << (k ? "," : "") << "\"" << kViolNames[k] << "\":" << s.violation[k];
  o << "},\"events_on_unbooked_orders\":" << s.events_on_unbooked << ",\"book_rebuilds_after_disagreement\":" << s.rebuilds
    << ",\"reference_comparisons\":" << s.ref_compares << ",\"reference_level_mismatches\":" << s.ref_level_mismatch
    << ",\"reference_queue_order_mismatches\":" << s.ref_queue_mismatch << "}";
}
}  // namespace

std::string Validator::to_json(const std::string& file_label) const {
  std::ostringstream o;
  o << "{\"input\":\"" << file_label << "\",\"first_timestamp_ns\":" << first_ts_ << ",\"last_timestamp_ns\":" << last_ts_;
  o << ",\"system_events_first_ts_ns\":{";
  bool first = true;
  for (const auto& [code, ts] : system_events_) {
    o << (first ? "" : ",") << "\"" << code << "\":" << ts;
    first = false;
  }
  o << "},\"messages_by_type\":{";
  first = true;
  for (int c = 0; c < 256; ++c)
    if (by_type_[static_cast<std::size_t>(c)]) {
      o << (first ? "" : ",") << "\"" << static_cast<char>(c) << "\":" << by_type_[static_cast<std::size_t>(c)];
      first = false;
    }
  o << "},\"total\":";
  stats_json(o, total());
  const auto syms = symbols();
  std::uint64_t opened = 0, diverged = 0;
  for (const auto& s : syms) {
    opened += s.opened;
    diverged += s.diverged;
  }
  o << ",\"symbols_seen\":" << syms.size() << ",\"symbols_with_opening_cross\":" << opened << ",\"symbols_diverged_rebuild_cap\":" << diverged
    << ",\"symbols\":[";
  std::size_t shown = 0;
  for (const auto& s : syms) {
    if (!(s.reference || shown < 25)) continue;
    o << (shown ? "," : "") << "{\"symbol\":\"" << s.name << "\",\"reference_model_compared\":" << (s.reference ? "true" : "false")
      << ",\"opening_cross\":" << (s.opened ? "true" : "false") << ",\"closing_cross\":" << (s.closed ? "true" : "false")
      << ",\"resting_orders_at_end\":" << s.resting_orders << ",\"stats\":";
    stats_json(o, s.stats);
    o << "}";
    ++shown;
  }
  o << "],\"mismatch_examples\":[";
  for (std::size_t i = 0; i < examples_.size(); ++i) {
    const auto& e = examples_[i];
    o << (i ? "," : "") << "{\"symbol\":\"" << e.symbol << "\",\"ts_ns\":" << e.ts_ns << ",\"bucket\":\"" << kBucketNames[e.bucket] << "\",\"kind\":\""
      << kKindNames[e.kind] << "\",\"exchange_executed_ref\":" << e.ref << ",\"side\":\"" << e.side << "\",\"price\":" << e.price
      << ",\"executed\":" << e.executed << ",\"order_original_shares\":" << e.order_shares_orig << ",\"engine_chose_ref\":" << e.other_ref
      << ",\"engine_price\":" << e.other_price << ",\"engine_choice_original_shares\":" << e.other_shares_orig << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace lob::itch
