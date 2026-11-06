#include "lob/replay_io.hpp"

#include <charconv>
#include <cstdio>
#include <sstream>
#include <vector>

namespace lob::sim {

namespace {

template <class T>
bool to_int(std::string_view s, T& out) {
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && p == s.data() + s.size();
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool decode(char op, std::string_view side, std::string_view type, std::string_view tif, TimedOp& out) {
  Op& o = out.op;
  if (op == 'S') o.kind = Op::Kind::Submit;
  else if (op == 'C') o.kind = Op::Kind::Cancel;
  else if (op == 'M') o.kind = Op::Kind::Modify;
  else return false;
  if (o.kind == Op::Kind::Submit) {
    if (side == "B") o.side = Side::Buy;
    else if (side == "S") o.side = Side::Sell;
    else return false;
    if (type == "L") o.type = OrderType::Limit;
    else if (type == "M") o.type = OrderType::Market;
    else return false;
    if (tif == "GTC") o.tif = TimeInForce::GTC;
    else if (tif == "IOC") o.tif = TimeInForce::IOC;
    else if (tif == "FOK") o.tif = TimeInForce::FOK;
    else return false;
  }
  return true;
}

char op_char(const Op& o) { return o.kind == Op::Kind::Submit ? 'S' : o.kind == Op::Kind::Cancel ? 'C' : 'M'; }
const char* tif_str(TimeInForce t) { return t == TimeInForce::GTC ? "GTC" : t == TimeInForce::IOC ? "IOC" : "FOK"; }

}  // namespace

std::string csv_header() { return "t_ns,op,id,side,type,tif,price,qty"; }

std::string to_csv_line(const TimedOp& t) {
  const Op& o = t.op;
  std::ostringstream s;
  s << t.t_ns << ',' << op_char(o) << ',' << o.id << ',';
  if (o.kind == Op::Kind::Submit)
    s << (o.side == Side::Buy ? 'B' : 'S') << ',' << (o.type == OrderType::Limit ? 'L' : 'M') << ',' << tif_str(o.tif) << ',';
  else
    s << ",,,";
  if (o.kind == Op::Kind::Cancel) s << ',';
  else s << o.price << ',';
  if (o.kind != Op::Kind::Cancel) s << o.qty;
  return s.str();
}

std::optional<TimedOp> parse_csv_line(std::string_view line) {
  line = trim(line);
  if (line.empty() || line[0] == '#' || line.rfind("t_ns", 0) == 0) return std::nullopt;
  std::vector<std::string_view> f;
  std::size_t start = 0;
  while (true) {
    const std::size_t c = line.find(',', start);
    f.push_back(trim(line.substr(start, c == std::string_view::npos ? c : c - start)));
    if (c == std::string_view::npos) break;
    start = c + 1;
  }
  if (f.size() != 8 || f[1].size() != 1) return std::nullopt;
  TimedOp t;
  if (!to_int(f[0], t.t_ns) || !to_int(f[2], t.op.id)) return std::nullopt;
  if (!decode(f[1][0], f[3], f[4], f[5], t)) return std::nullopt;
  if (t.op.kind != Op::Kind::Cancel && (!to_int(f[6], t.op.price) || !to_int(f[7], t.op.qty))) return std::nullopt;
  return t;
}

std::string to_jsonl_line(const TimedOp& t) {
  const Op& o = t.op;
  std::ostringstream s;
  s << "{\"t\":" << t.t_ns << ",\"op\":\"" << op_char(o) << "\",\"id\":" << o.id;
  if (o.kind == Op::Kind::Submit)
    s << ",\"side\":\"" << (o.side == Side::Buy ? 'B' : 'S') << "\",\"type\":\"" << (o.type == OrderType::Limit ? 'L' : 'M')
      << "\",\"tif\":\"" << tif_str(o.tif) << "\"";
  if (o.kind != Op::Kind::Cancel) s << ",\"price\":" << o.price << ",\"qty\":" << o.qty;
  s << '}';
  return s.str();
}

// Parses a flat object of "key":value pairs where value is a quoted string or an integer.
std::optional<TimedOp> parse_jsonl_line(std::string_view line) {
  line = trim(line);
  if (line.size() < 2 || line.front() != '{' || line.back() != '}') return std::nullopt;
  std::string t, op, id, side, type, tif, price, qty;
  std::size_t i = 1;
  while (i < line.size() - 1) {
    while (i < line.size() && (line[i] == ' ' || line[i] == ',')) ++i;
    if (i >= line.size() - 1) break;
    if (line[i] != '"') return std::nullopt;
    const std::size_t k0 = ++i;
    while (i < line.size() && line[i] != '"') ++i;
    if (i >= line.size()) return std::nullopt;
    const std::string_view key = line.substr(k0, i - k0);
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == ':')) ++i;
    std::string val;
    if (i < line.size() && line[i] == '"') {
      const std::size_t v0 = ++i;
      while (i < line.size() && line[i] != '"') ++i;
      if (i >= line.size()) return std::nullopt;
      val = std::string(line.substr(v0, i - v0));
      ++i;
    } else {
      const std::size_t v0 = i;
      while (i < line.size() && line[i] != ',' && line[i] != '}' && line[i] != ' ') ++i;
      val = std::string(line.substr(v0, i - v0));
    }
    if (key == "t") t = val;
    else if (key == "op") op = val;
    else if (key == "id") id = val;
    else if (key == "side") side = val;
    else if (key == "type") type = val;
    else if (key == "tif") tif = val;
    else if (key == "price") price = val;
    else if (key == "qty") qty = val;
  }
  if (op.size() != 1) return std::nullopt;
  TimedOp r;
  if (!to_int(t, r.t_ns) || !to_int(id, r.op.id)) return std::nullopt;
  if (!decode(op[0], side, type, tif, r)) return std::nullopt;
  if (r.op.kind != Op::Kind::Cancel && (!to_int(price, r.op.price) || !to_int(qty, r.op.qty))) return std::nullopt;
  return r;
}

std::string run_to_json(const Run& r) {
  const MarketConfig& c = r.config;
  std::uint64_t submits = 0, cancels = 0, modifies = 0, trades = 0, rejects = 0;
  for (const auto& t : r.ops) {
    if (t.op.kind == Op::Kind::Submit) ++submits;
    else if (t.op.kind == Op::Kind::Cancel) ++cancels;
    else ++modifies;
  }
  for (const auto& e : r.events) {
    if (e.ev.type == EventType::Trade) ++trades;
    if (e.ev.type == EventType::Rejected) ++rejects;
  }
  char hex[32];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(r.checksum));
  char buf[64];
  std::ostringstream o;
  o << "{\"format\":\"lob-run-v1\",\n\"meta\":{\"source\":\"" << r.source << "\",\"seed\":" << c.seed << ",\"ops\":" << r.ops.size();
  std::snprintf(buf, sizeof buf, "%.10g", c.arrival_rate_per_sec);
  o << ",\"arrival_rate_per_sec\":" << buf;
  std::snprintf(buf, sizeof buf, "%.10g", c.cancel_fraction);
  o << ",\"cancel_fraction\":" << buf;
  std::snprintf(buf, sizeof buf, "%.10g", c.modify_fraction);
  o << ",\"modify_fraction\":" << buf;
  std::snprintf(buf, sizeof buf, "%.10g", c.aggressiveness);
  o << ",\"aggressiveness\":" << buf;
  std::snprintf(buf, sizeof buf, "%.10g", c.drift_prob);
  o << ",\"drift_prob\":" << buf;
  o << ",\"depth_dist\":\"" << to_string(c.depth_dist) << "\",\"depth_mean_ticks\":" << c.depth_mean_ticks
    << ",\"initial_mid\":" << c.initial_mid << ",\"max_qty\":" << c.max_qty << ",\"book_target\":" << c.book_target
    << ",\"snapshot_every\":" << r.snapshot_every << ",\"depth_levels\":" << r.depth_levels
    << ",\"duration_ns\":" << (r.ops.empty() ? 0 : r.ops.back().t_ns) << ",\"counts\":{\"submits\":" << submits
    << ",\"cancels\":" << cancels << ",\"modifies\":" << modifies << ",\"events\":" << r.events.size() << ",\"trades\":" << trades
    << ",\"rejects\":" << rejects << "},\"checksum\":\"" << hex << "\"},\n";
  o << "\"ops_schema\":[\"t_ns\",\"kind(0=submit,1=cancel,2=modify)\",\"id\",\"side(0=buy,1=sell)\",\"type(0=limit,1=market)\",\"tif(0=GTC,1=IOC,2=FOK)\",\"price\",\"qty\"],\n\"ops\":[";
  for (std::size_t i = 0; i < r.ops.size(); ++i) {
    const auto& t = r.ops[i];
    const Op& p = t.op;
    o << (i ? ",\n" : "\n") << '[' << t.t_ns << ',' << static_cast<int>(p.kind) << ',' << p.id << ',' << static_cast<int>(p.side) << ','
      << static_cast<int>(p.type) << ',' << static_cast<int>(p.tif) << ',' << p.price << ',' << p.qty << ']';
  }
  o << "],\n\"events_schema\":[\"op_index\",\"type(0=accepted,1=rejected,2=trade,3=cancelled,4=modified)\",\"order_id\",\"maker_id\",\"side(0=buy,1=sell)\",\"price\",\"qty\",\"aux(reject reason | cancel reason | lost_priority)\"],\n\"events\":[";
  for (std::size_t i = 0; i < r.events.size(); ++i) {
    const auto& te = r.events[i];
    const Event& e = te.ev;
    int aux = 0;
    if (e.type == EventType::Rejected) aux = static_cast<int>(e.reject);
    else if (e.type == EventType::Cancelled) aux = static_cast<int>(e.cancel);
    else if (e.type == EventType::Modified) aux = e.lost_priority ? 1 : 0;
    o << (i ? ",\n" : "\n") << '[' << te.op_index << ',' << static_cast<int>(e.type) << ',' << e.order_id << ',' << e.maker_id << ','
      << static_cast<int>(e.side) << ',' << e.price << ',' << e.qty << ',' << aux << ']';
  }
  o << "],\n\"snapshots_schema\":[\"op_index\",\"t_ns\",\"bids[[price,qty,orders]...] best first\",\"asks[[price,qty,orders]...] best first\"],\n\"snapshots\":[";
  auto levels = [&](const std::vector<LevelInfo>& v) {
    o << '[';
    for (std::size_t i = 0; i < v.size(); ++i) o << (i ? "," : "") << '[' << v[i].price << ',' << v[i].qty << ',' << v[i].order_count << ']';
    o << ']';
  };
  for (std::size_t i = 0; i < r.snapshots.size(); ++i) {
    const auto& s = r.snapshots[i];
    o << (i ? ",\n" : "\n") << '[' << s.op_index << ',' << s.t_ns << ',';
    levels(s.bids);
    o << ',';
    levels(s.asks);
    o << ']';
  }
  o << "]}\n";
  return o.str();
}

std::string run_to_js(const Run& run) { return "window.LOB_RUN = " + run_to_json(run) + ";\n"; }

}  // namespace lob::sim
