#include "itch_synth.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include "lob/market_sim.hpp"

namespace lob::itch {

namespace {

struct Out {
  std::uint64_t ts;
  std::uint64_t order;  // tie-break to keep generation order within a timestamp
  Message m;
};

Message sysmsg(char code, std::uint64_t ts) {
  Message m;
  m.type = 'S';
  m.code = code;
  m.ts_ns = ts;
  return m;
}

void set_stock(Message& m, const std::string& s) {
  std::snprintf(m.stock, sizeof m.stock, "%s", s.c_str());
}

}  // namespace

std::vector<std::uint8_t> make_synthetic_itch(const SynthConfig& cfg) {
  std::vector<Out> body;
  std::uint64_t order = 0, match = 1000, next_new_ref = 1ULL << 40;
  std::vector<std::string> names;

  for (int s = 0; s < cfg.symbols; ++s) {
    const std::string name = std::string("SYN") + static_cast<char>('A' + s);
    names.push_back(name);
    const std::uint16_t locate = static_cast<std::uint16_t>(s + 1);

    sim::MarketConfig mc;
    mc.seed = cfg.seed * 1000 + static_cast<std::uint64_t>(s);
    mc.ops = cfg.ops_per_symbol;
    mc.arrival_rate_per_sec = 50000;
    mc.initial_mid = 100000 + 5000 * s;  // 10.00 and 10.50 dollars in 1/10000
    mc.book_target = 150;
    mc.aggressiveness = 0.2;
    const sim::Run run = sim::simulate(mc, 1000000, 1);

    // Simulator order id -> current ITCH reference, remaining shares and price.
    struct Live {
      std::uint64_t ref;
      std::int64_t remaining, price;
      char side;
    };
    std::unordered_map<std::uint64_t, Live> live;
    std::size_t ev = 0;
    for (std::size_t i = 0; i < run.ops.size(); ++i) {
      const auto& t = run.ops[i];
      const std::uint64_t ts = cfg.base_ts_ns + 1000 + t.t_ns;
      // events of this op
      std::size_t ev_end = ev;
      while (ev_end < run.events.size() && run.events[ev_end].op_index == i) ++ev_end;
      auto emit = [&](Message m) {
        m.locate = locate;
        m.ts_ns = ts;
        body.push_back({ts, order++, m});
      };
      // executions of resting orders first (they precede the residual add in a real feed)
      std::int64_t traded = 0;
      for (std::size_t k = ev; k < ev_end; ++k) {
        const Event& e = run.events[k].ev;
        if (e.type != EventType::Trade) continue;
        auto mit = live.find(e.maker_id);
        if (mit == live.end()) {
          std::fprintf(stderr, "synth: maker %llu not live at op %zu kind %d tif %d type %d\n", (unsigned long long)e.maker_id, i, (int)t.op.kind, (int)t.op.tif, (int)t.op.type);
          continue;
        }
        Live& mk = mit->second;
        Message m;
        m.type = 'E';
        m.ref = mk.ref;
        m.qty = static_cast<std::uint64_t>(e.qty);
        m.match = match++;
        emit(m);
        mk.remaining -= e.qty;
        traded += e.qty;
        if (mk.remaining == 0) live.erase(e.maker_id);
      }
      const Op& op = t.op;
      if (op.kind == Op::Kind::Submit) {
        // rests only if a GTC limit with a remainder
        bool rests = false;
        std::int64_t rem = op.qty - traded;
        if (op.type == OrderType::Limit && op.tif == TimeInForce::GTC && rem > 0) rests = true;
        if (rests) {
          Message m;
          m.type = 'A';
          m.ref = (static_cast<std::uint64_t>(s + 1) << 32) + op.id;  // references are unique across symbols
          m.side = op.side == Side::Buy ? 'B' : 'S';
          m.qty = static_cast<std::uint64_t>(rem);
          m.price = op.price;
          set_stock(m, name);
          emit(m);
          live[op.id] = {m.ref, rem, op.price, m.side};
        }
      } else if (op.kind == Op::Kind::Cancel) {
        auto it = live.find(op.id);
        if (it != live.end()) {
          Message m;
          m.type = 'D';
          m.ref = it->second.ref;
          emit(m);
          live.erase(it);
        }
      } else {  // modify
        auto it = live.find(op.id);
        if (it != live.end()) {
          Live& l = it->second;
          if (op.price == l.price && op.qty < l.remaining) {
            Message m;
            m.type = 'X';
            m.ref = l.ref;
            m.qty = static_cast<std::uint64_t>(l.remaining - op.qty);
            emit(m);
            l.remaining = op.qty;
          } else if (op.price == l.price && op.qty == l.remaining) {
            // no-op in the engine, nothing on the wire
          } else {
            // replace: new reference; any trades the modified order made were emitted above, and
            // the remainder re-rests
            const std::int64_t rem = op.qty - traded;
            Message m;
            m.type = 'U';
            m.ref = l.ref;
            m.new_ref = next_new_ref++;
            m.qty = static_cast<std::uint64_t>(std::max<std::int64_t>(rem, 1));
            m.price = op.price;
            if (rem > 0) {
              emit(m);
              l.ref = m.new_ref;
              l.remaining = rem;
              l.price = op.price;
            } else {
              Message d;
              d.type = 'D';
              d.ref = l.ref;
              emit(d);
              live.erase(it);
            }
          }
        }
      }
      ev = ev_end;
    }
  }

  std::stable_sort(body.begin(), body.end(), [](const Out& a, const Out& b) { return a.ts != b.ts ? a.ts < b.ts : a.order < b.order; });

  std::vector<std::uint8_t> out;
  const std::uint64_t t0 = cfg.base_ts_ns;
  append_framed(out, sysmsg('O', t0 - 5'000'000'000ULL));
  append_framed(out, sysmsg('S', t0 - 4'000'000'000ULL));
  for (std::size_t s = 0; s < names.size(); ++s) {
    Message r;
    r.type = 'R';
    r.locate = static_cast<std::uint16_t>(s + 1);
    r.ts_ns = t0 - 3'000'000'000ULL;
    set_stock(r, names[s]);
    append_framed(out, r);
    Message h;
    h.type = 'H';
    h.locate = r.locate;
    h.ts_ns = t0 - 2'000'000'000ULL;
    h.code = 'T';
    set_stock(h, names[s]);
    append_framed(out, h);
  }
  append_framed(out, sysmsg('Q', t0 - 100));
  for (std::size_t s = 0; s < names.size(); ++s) {
    Message q;
    q.type = 'Q';
    q.locate = static_cast<std::uint16_t>(s + 1);
    q.ts_ns = t0;
    q.code = 'O';
    q.qty = 1000;
    q.price = 100000;
    q.match = 1;
    set_stock(q, names[s]);
    append_framed(out, q);
  }
  std::uint64_t last_ts = t0;
  for (const auto& o : body) {
    append_framed(out, o.m);
    last_ts = std::max(last_ts, o.ts);
  }
  for (std::size_t s = 0; s < names.size(); ++s) {
    Message q;
    q.type = 'Q';
    q.locate = static_cast<std::uint16_t>(s + 1);
    q.ts_ns = last_ts + 1000;
    q.code = 'C';
    q.qty = 1000;
    q.price = 100000;
    q.match = 2;
    set_stock(q, names[s]);
    append_framed(out, q);
  }
  append_framed(out, sysmsg('M', last_ts + 2000));
  append_framed(out, sysmsg('E', last_ts + 3000));
  append_framed(out, sysmsg('C', last_ts + 4000));
  return out;
}

}  // namespace lob::itch
