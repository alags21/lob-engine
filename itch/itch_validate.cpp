#include "itch_validate.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <tuple>

#include "lob/checksum.hpp"

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
  tax_ahead_all_odd += o.tax_ahead_all_odd; tax_ahead_has_round += o.tax_ahead_has_round;
  tax_r_attributed += o.tax_r_attributed; tax_w_attributed += o.tax_w_attributed; tax_r_from_replace += o.tax_r_from_replace; tax_w_from_replace += o.tax_w_from_replace;
  tax_r_odd += o.tax_r_odd; tax_w_odd += o.tax_w_odd; tax_both_round += o.tax_both_round;
  tax_gap_le_1us += o.tax_gap_le_1us; tax_gap_le_1ms += o.tax_gap_le_1ms; tax_gap_le_1s += o.tax_gap_le_1s; tax_gap_gt_1s += o.tax_gap_gt_1s;
  tax_w_exec_le_1us += o.tax_w_exec_le_1us; tax_w_exec_le_1ms += o.tax_w_exec_le_1ms; tax_w_exec_later += o.tax_w_exec_later;
  tax_w_changed += o.tax_w_changed; tax_w_pending_end += o.tax_w_pending_end;
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
  std::unordered_map<std::uint64_t, std::uint64_t> pending_w;  // ITCH ref of a skipped-ahead order -> mismatch timestamp
  char state = 'T';
  bool opened = false, closed = false, diverged = false, selected = true;
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
    p->selected = cfg_.only_symbols.empty();
    p->make_book();
  }
  return *p;
}

void Validator::emit_op(const Symbol& s, const lob::Op& op) {
  if (cfg_.op_sink) cfg_.op_sink(s.locate, op);
}

int Validator::bucket_of(const Symbol& s) const {
  if (phase_ == 0) return kPre;
  if (phase_ == 2) return kPost;
  return (s.state == 'T' && s.opened && !s.closed) ? kContinuous : kMarketOther;
}

void Validator::add_order(Symbol& s, std::uint64_t ref, char side, std::int64_t price, std::uint32_t shares, bool attributed, bool from_replace) {
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
  o.add_ts = last_ts_;
  o.eng_id = ref;
  o.attributed = attributed;
  o.from_replace = from_replace;
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
      {
        lob::Op op;
        op.kind = lob::Op::Kind::Submit;
        op.id = ref;
        op.side = sd;
        op.price = price;
        op.qty = static_cast<Qty>(shares);
        emit_op(s, op);
      }
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
  if (cfg_.reset_sink) cfg_.reset_sink(s.locate);
  for (const auto& [price, seq, r] : v) {
    const Order& o = orders_.at(r);
    {
      lob::Op op;
      op.kind = lob::Op::Kind::Submit;
      op.id = o.eng_id;
      op.side = o.side == 'B' ? Side::Buy : Side::Sell;
      op.price = price;
      op.qty = static_cast<Qty>(o.remaining);
      emit_op(s, op);
    }
    s.book->submit({o.eng_id, o.side == 'B' ? Side::Buy : Side::Sell, OrderType::Limit, TimeInForce::GTC, price, static_cast<Qty>(o.remaining)});
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
  if (!s.pending_w.empty()) {
    const auto pw = s.pending_w.find(m.ref);
    if (pw != s.pending_w.end()) {
      const std::uint64_t dt = m.ts_ns - pw->second;
      if (dt <= 1000) ++s.stats.tax_w_exec_le_1us;
      else if (dt <= 1000000) ++s.stats.tax_w_exec_le_1ms;
      else ++s.stats.tax_w_exec_later;
      s.pending_w.erase(pw);
    }
  }
  auto it = orders_.find(m.ref);
  if (it == orders_.end()) {
    ++s.stats.violation[kUnknownRef];
    return;
  }
  Order& o = it->second;
  const std::uint64_t eid = o.eng_id;
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
    {
      lob::Op op;
      op.kind = lob::Op::Kind::Submit;
      op.id = ioc_id_;
      op.side = opposite(resting);
      op.tif = TimeInForce::IOC;
      op.price = o.price;
      op.qty = static_cast<Qty>(executed);
      emit_op(s, op);
    }
    s.book->submit({ioc_id_++, opposite(resting), OrderType::Limit, TimeInForce::IOC, o.price, static_cast<Qty>(executed)});
    std::vector<Event> trades;
    for (const auto& e : s.ev)
      if (e.type == EventType::Trade) trades.push_back(e);
    const bool ok = trades.size() == 1 && trades[0].maker_id == eid && trades[0].price == o.price &&
                    trades[0].qty == static_cast<Qty>(executed);
    if (ok) {
      ++s.stats.exec_ok[b];
    } else {
      int kind = kOtherDifference;
      if (trades.empty()) kind = kNoFill;
      else if (trades[0].maker_id != eid) {
        const bool better = resting == Side::Sell ? trades[0].price < o.price : trades[0].price > o.price;
        kind = better ? kBetterPriceAhead : (trades[0].price == o.price ? kSamePriceOtherOrder : kOtherDifference);
      }
      ++s.stats.mismatch[b][kind];
      const Order* other = nullptr;
      if (!trades.empty()) {
        const auto rn = renamed_.find(trades[0].maker_id);
        const std::uint64_t key = rn == renamed_.end() ? trades[0].maker_id : rn->second;
        const auto oi = orders_.find(key);
        if (oi != orders_.end()) other = &oi->second;
      }
      if (o.original < 100 || (other && other->original < 100)) ++s.stats.mismatch_odd_lot[b];
      if (b == kContinuous && other && kind == kSamePriceOtherOrder) {
        const bool ro = o.original < 100, wo = other->original < 100;
        {
          bool has_round = false;
          for (std::uint64_t lr : s.live) {
            const Order& q = orders_.at(lr);
            if (&q == &o || q.side != o.side || q.price != o.price || !q.booked || q.seq >= o.seq) continue;
            if (q.original >= 100) {
              has_round = true;
              break;
            }
          }
          if (has_round) ++s.stats.tax_ahead_has_round;
          else ++s.stats.tax_ahead_all_odd;
        }
        s.stats.tax_r_attributed += o.attributed;
        s.stats.tax_w_attributed += other->attributed;
        s.stats.tax_r_from_replace += o.from_replace;
        s.stats.tax_w_from_replace += other->from_replace;
        s.stats.tax_r_odd += ro;
        s.stats.tax_w_odd += wo;
        s.stats.tax_both_round += (!ro && !wo);
        const std::uint64_t gap = o.add_ts > other->add_ts ? o.add_ts - other->add_ts : other->add_ts - o.add_ts;
        if (gap <= 1000) ++s.stats.tax_gap_le_1us;
        else if (gap <= 1000000) ++s.stats.tax_gap_le_1ms;
        else if (gap <= 1000000000ULL) ++s.stats.tax_gap_le_1s;
        else ++s.stats.tax_gap_gt_1s;
        const auto rn2 = renamed_.find(trades[0].maker_id);
        s.pending_w[rn2 == renamed_.end() ? trades[0].maker_id : rn2->second] = m.ts_ns;
      }
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
      if (new_rem == 0) s.ref->cancel(eid);
      else s.ref->modify(eid, o.price, static_cast<Qty>(new_rem));
    }
    o.remaining = new_rem;
    if (new_rem == 0) {
      renamed_.erase(eid);
      remove_order(s, m.ref, o);
    }
    if (!ok) {
      rebuild(s);
    } else if (new_rem > 0) {
      const auto v = s.book->find(eid);
      if (!v || v->qty != static_cast<Qty>(new_rem)) ++s.stats.violation[kBookQtyMismatch];
    }
    return;
  }
  // unbooked path: tracker only (the reference mirror never saw this order either)
  o.remaining = new_rem;
  if (new_rem == 0) {
    renamed_.erase(eid);
    remove_order(s, m.ref, o);
  }
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
  if (m.type != 'R' && !s.selected) return;
  ++s.stats.messages;
  ++s.msgs;
  switch (m.type) {
    case 'R':
      s.name = m.stock;
      s.selected = cfg_.only_symbols.empty() || cfg_.only_symbols.count(s.name) > 0;
      if (cfg_.reference_symbols.count(s.name) && !s.ref) s.ref = std::make_unique<lob::ref::ReferenceBook>();
      break;
    case 'H':
      ++s.stats.trading_actions;
      s.state = m.code;
      break;
    case 'A':
    case 'F':
      ++s.stats.adds;
      add_order(s, m.ref, m.side, m.price, static_cast<std::uint32_t>(m.qty), m.type == 'F', false);
      break;
    case 'D': {
      ++s.stats.deletes;
      if (!s.pending_w.empty() && s.pending_w.erase(m.ref)) ++s.stats.tax_w_changed;
      auto it = orders_.find(m.ref);
      if (it == orders_.end()) {
        ++s.stats.violation[kUnknownRef];
        break;
      }
      Order& o = it->second;
      if (o.booked && !s.diverged) {
        s.ev.clear();
        {
          lob::Op op;
          op.kind = lob::Op::Kind::Cancel;
          op.id = o.eng_id;
          emit_op(s, op);
        }
        if (s.book->cancel(o.eng_id) != RejectReason::None) ++s.stats.violation[kBookRejected];
        if (s.ref) s.ref->cancel(o.eng_id);
      } else {
        ++s.stats.events_on_unbooked;
      }
      renamed_.erase(o.eng_id);
      remove_order(s, m.ref, o);
      break;
    }
    case 'X': {
      ++s.stats.partial_cancels;
      if (!s.pending_w.empty() && s.pending_w.erase(m.ref)) ++s.stats.tax_w_changed;
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
        {
          lob::Op op;
          op.kind = new_rem == 0 ? lob::Op::Kind::Cancel : lob::Op::Kind::Modify;
          op.id = o.eng_id;
          op.price = o.price;
          op.qty = static_cast<Qty>(new_rem);
          emit_op(s, op);
        }
        if (new_rem == 0) {
          if (s.book->cancel(o.eng_id) != RejectReason::None) ++s.stats.violation[kBookRejected];
          if (s.ref) s.ref->cancel(o.eng_id);
        } else {
          if (cfg_.partial_cancel_loses_priority) {
            s.book->cancel(o.eng_id);
            s.book->submit({o.eng_id, o.side == 'B' ? Side::Buy : Side::Sell, OrderType::Limit, TimeInForce::GTC, o.price, static_cast<Qty>(new_rem)});
            if (s.ref) {
              s.ref->cancel(o.eng_id);
              s.ref->submit({o.eng_id, o.side == 'B' ? Side::Buy : Side::Sell, OrderType::Limit, TimeInForce::GTC, o.price, static_cast<Qty>(new_rem)});
            }
            o.seq = ++seq_;
          } else {
            if (s.book->modify(o.eng_id, o.price, static_cast<Qty>(new_rem)) != RejectReason::None) ++s.stats.violation[kBookRejected];
            if (s.ref) s.ref->modify(o.eng_id, o.price, static_cast<Qty>(new_rem));
          }
          const auto v = s.book->find(o.eng_id);
          if (!v || v->qty != static_cast<Qty>(new_rem)) ++s.stats.violation[kBookQtyMismatch];
        }
      } else {
        ++s.stats.events_on_unbooked;
      }
      o.remaining = new_rem;
      if (new_rem == 0) {
        renamed_.erase(o.eng_id);
        remove_order(s, m.ref, o);
      }
      break;
    }
    case 'U': {
      ++s.stats.replaces;
      if (!s.pending_w.empty() && s.pending_w.erase(m.ref)) ++s.stats.tax_w_changed;
      auto it = orders_.find(m.ref);
      if (it == orders_.end()) {
        ++s.stats.violation[kUnknownRef];
        break;
      }
      Order& o = it->second;
      const char side = o.side;
      if (cfg_.replace_reduction_keeps_priority && o.booked && !s.diverged && m.price == o.price && m.qty > 0 && m.qty <= o.remaining &&
          !orders_.count(m.new_ref)) {
        // in place: same price, size not increased -> keeps queue priority; the exchange gives it a new reference
        s.ev.clear();
        Order moved = o;
        if (m.qty < o.remaining) {
          lob::Op op;
          op.kind = lob::Op::Kind::Modify;
          op.id = o.eng_id;
          op.price = o.price;
          op.qty = static_cast<Qty>(m.qty);
          emit_op(s, op);
          if (s.book->modify(o.eng_id, o.price, static_cast<Qty>(m.qty)) != RejectReason::None) ++s.stats.violation[kBookRejected];
          if (s.ref) s.ref->modify(o.eng_id, o.price, static_cast<Qty>(m.qty));
        }
        moved.remaining = static_cast<std::uint32_t>(m.qty);
        moved.original = moved.remaining;
        s.live.erase(m.ref);
        orders_.erase(m.ref);
        renamed_[moved.eng_id] = m.new_ref;
        orders_.emplace(m.new_ref, moved);
        s.live.insert(m.new_ref);
        break;
      }
      if (o.booked && !s.diverged) {
        s.ev.clear();
        {
          lob::Op op;
          op.kind = lob::Op::Kind::Cancel;
          op.id = o.eng_id;
          emit_op(s, op);
        }
        if (s.book->cancel(o.eng_id) != RejectReason::None) ++s.stats.violation[kBookRejected];
        if (s.ref) s.ref->cancel(o.eng_id);
      } else {
        ++s.stats.events_on_unbooked;
      }
      renamed_.erase(o.eng_id);
      remove_order(s, m.ref, o);
      add_order(s, m.new_ref, side, m.price, static_cast<std::uint32_t>(m.qty), false, true);
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
  for (auto& [loc, s] : syms_) {
    s->stats.tax_w_pending_end += s->pending_w.size();
    s->pending_w.clear();
    if (s->ref) compare_reference(*s);
  }
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

std::uint64_t Validator::book_digest(const std::string& symbol) const {
  for (const auto& [loc, s] : syms_)
    if (s->name == symbol) return lob::digest_book(14695981039346656037ULL, *s->book);
  return 0;
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
    << ",\"reference_queue_order_mismatches\":" << s.ref_queue_mismatch
    << ",\"continuous_same_price_mismatch_taxonomy\":{\"orders_ahead_of_executed_all_odd_lots\":" << s.tax_ahead_all_odd << ",\"orders_ahead_of_executed_include_a_round_lot\":" << s.tax_ahead_has_round << ",\"executed_order_is_attributed_F\":" << s.tax_r_attributed << ",\"skipped_order_is_attributed_F\":" << s.tax_w_attributed << ",\"executed_order_created_by_replace_U\":" << s.tax_r_from_replace << ",\"skipped_order_created_by_replace_U\":" << s.tax_w_from_replace << ",\"executed_order_is_odd_lot\":" << s.tax_r_odd << ",\"skipped_order_is_odd_lot\":" << s.tax_w_odd
    << ",\"both_round_lot\":" << s.tax_both_round << ",\"gap_between_the_two_adds_le_1us\":" << s.tax_gap_le_1us << ",\"gap_le_1ms\":" << s.tax_gap_le_1ms
    << ",\"gap_le_1s\":" << s.tax_gap_le_1s << ",\"gap_gt_1s\":" << s.tax_gap_gt_1s << ",\"skipped_order_then_executed_within_1us\":" << s.tax_w_exec_le_1us
    << ",\"skipped_order_then_executed_within_1ms\":" << s.tax_w_exec_le_1ms << ",\"skipped_order_then_executed_later\":" << s.tax_w_exec_later
    << ",\"skipped_order_then_cancelled_or_modified\":" << s.tax_w_changed << ",\"skipped_order_still_resting_at_end\":" << s.tax_w_pending_end << "}}";
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
