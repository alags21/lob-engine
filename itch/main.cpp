#define _CRT_SECURE_NO_WARNINGS 1
// lob_itch: ITCH 5.0 tool.
//
//   lob_itch synth   --out FILE [--seed S] [--ops N] [--symbols K]     write a synthetic ITCH file
//   lob_itch scan    --input FILE --json OUT                           message counts per type and per symbol
//   lob_itch validate --input FILE --json OUT [--reference AAPL,MSFT] [--compare-every N] [--max-messages N]
//   lob_itch bench   --input FILE --json OUT --symbols AAPL,MSFT,... [--reps R] [--cpu C] ...  (see bench.cpp)
//
// FILE may be a raw ITCH BinaryFILE or, when built with zlib, a .gz file which is decompressed as a
// stream (the full day is never written to disk). The input stays wherever it is; nothing here
// copies Nasdaq data into the repository.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "itch_synth.hpp"
#include "bench.hpp"
#include "itch_validate.hpp"
#include "lob/itch.hpp"
#include "tool_io.hpp"

using namespace lob::itch;



namespace {

struct Args {
  std::map<std::string, std::string> kv;
  std::string get(const std::string& k, const std::string& d = "") const {
    auto it = kv.find(k);
    return it == kv.end() ? d : it->second;
  }
  std::uint64_t num(const std::string& k, std::uint64_t d) const {
    auto it = kv.find(k);
    return it == kv.end() ? d : std::strtoull(it->second.c_str(), nullptr, 10);
  }
};

std::set<std::string> split_csv(const std::string& s) {
  std::set<std::string> out;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) out.insert(tok);
  }
  return out;
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary);
  f << content;
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    std::exit(1);
  }
}

int cmd_synth(const Args& a) {
  SynthConfig c;
  c.seed = a.num("seed", 1);
  c.ops_per_symbol = a.num("ops", 4000);
  c.symbols = static_cast<int>(a.num("symbols", 2));
  const auto bytes = make_synthetic_itch(c);
  const std::string out = a.get("out");
  if (out.empty()) return 2;
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  std::printf("wrote %zu bytes to %s\n", bytes.size(), out.c_str());
  return 0;
}

int cmd_scan(const Args& a) {
  tool::FileSource src;
  if (!src.open(a.get("input"))) {
    std::fprintf(stderr, "cannot open input\n");
    return 2;
  }
  FrameReader<tool::FileSource> fr(src);
  const std::uint8_t* body;
  std::size_t len;
  std::map<std::uint16_t, std::string> names;
  std::map<std::uint16_t, std::uint64_t> counts, adds, execs;
  std::uint64_t by_type[256] = {}, malformed = 0, total = 0;
  std::map<char, std::uint64_t> sys;
  Message m;
  const auto t0 = std::chrono::steady_clock::now();
  while (fr.next(body, len)) {
    ++total;
    ++by_type[body[0]];
    const auto st = parse(body, len, m);
    if (st == ParseStatus::Malformed) {
      ++malformed;
      continue;
    }
    if (st != ParseStatus::Ok) continue;
    if (m.type == 'S') sys.emplace(m.code, m.ts_ns);
    else {
      ++counts[m.locate];
      if (m.type == 'R') names[m.locate] = m.stock;
      if (m.type == 'A' || m.type == 'F') ++adds[m.locate];
      if (m.type == 'E' || m.type == 'C') ++execs[m.locate];
    }
    if (total % 50000000 == 0) std::fprintf(stderr, "scan: %llu messages\n", static_cast<unsigned long long>(total));
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::vector<std::pair<std::uint64_t, std::uint16_t>> order;
  for (const auto& [loc, c] : counts) order.emplace_back(c, loc);
  std::sort(order.rbegin(), order.rend());
  std::ostringstream o;
  o << "{\"input\":\"" << a.get("input") << "\",\"messages\":" << total << ",\"malformed\":" << malformed << ",\"truncated_final_frame\":"
    << (fr.truncated() ? "true" : "false") << ",\"scan_seconds\":" << secs << ",\"symbols\":" << counts.size() << ",\"messages_by_type\":{";
  bool first = true;
  for (int c = 0; c < 256; ++c)
    if (by_type[c]) {
      o << (first ? "" : ",") << "\"" << static_cast<char>(c) << "\":" << by_type[c];
      first = false;
    }
  o << "},\"system_events_first_ts_ns\":{";
  first = true;
  for (const auto& [c, ts] : sys) {
    o << (first ? "" : ",") << "\"" << c << "\":" << ts;
    first = false;
  }
  o << "},\"symbols_by_messages\":[";
  for (std::size_t i = 0; i < order.size(); ++i) {
    const auto loc = order[i].second;
    o << (i ? "," : "") << "{\"symbol\":\"" << names[loc] << "\",\"locate\":" << loc << ",\"messages\":" << order[i].first << ",\"adds\":" << adds[loc]
      << ",\"executions\":" << execs[loc] << "}";
  }
  o << "]}";
  const std::string out = a.get("json");
  if (!out.empty()) write_file(out, o.str());
  std::printf("scan: %llu messages, %zu symbols, %.1f s\n", static_cast<unsigned long long>(total), counts.size(), secs);
  return 0;
}

int cmd_validate(const Args& a) {
  tool::FileSource src;
  if (!src.open(a.get("input"))) {
    std::fprintf(stderr, "cannot open input\n");
    return 2;
  }
  ValidateConfig cfg;
  cfg.reference_symbols = split_csv(a.get("reference"));
  cfg.compare_every = a.num("compare-every", 2000);
  cfg.replace_reduction_keeps_priority = a.num("replace-keeps-priority", 0) != 0;
  cfg.partial_cancel_loses_priority = a.num("x-loses-priority", 0) != 0;
  cfg.only_symbols = split_csv(a.get("only"));
  const std::uint64_t max_messages = a.num("max-messages", 0);
  Validator v(cfg);
  FrameReader<tool::FileSource> fr(src);
  const std::uint8_t* body;
  std::size_t len;
  Message m;
  std::uint64_t n = 0, unknown = 0, malformed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (fr.next(body, len)) {
    const auto st = parse(body, len, m);
    if (st == ParseStatus::Malformed) {
      ++malformed;
      continue;
    }
    if (st == ParseStatus::Unknown) {
      ++unknown;
      continue;
    }
    try {
      v.on_message(m);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "exception at message %llu type %c ref %llu locate %u: %s\n", static_cast<unsigned long long>(n), m.type, static_cast<unsigned long long>(m.ref), m.locate, e.what());
      return 3;
    }
    if (++n % 25000000 == 0) std::fprintf(stderr, "validate: %llu messages\n", static_cast<unsigned long long>(n));
    if (max_messages && n >= max_messages) break;
  }
  v.finish();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::string json = v.to_json(a.get("label", a.get("input")));
  json.pop_back();  // reopen the object to append run facts
  std::ostringstream extra;
  extra << ",\"decoded_messages\":" << n << ",\"unknown_type_messages_skipped\":" << unknown << ",\"malformed_messages\":" << malformed
        << ",\"truncated_final_frame\":" << (fr.truncated() ? "true" : "false") << ",\"validate_seconds_including_decompression\":" << secs
        << ",\"compare_every\":" << cfg.compare_every << ",\"replace_reduction_keeps_priority\":" << (cfg.replace_reduction_keeps_priority ? "true" : "false") << "}";
  const std::string out = a.get("json");
  if (!out.empty()) write_file(out, json + extra.str());
  std::printf("validate: %llu messages in %.1f s\n", static_cast<unsigned long long>(n), secs);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: lob_itch synth|scan|validate|bench ...\n");
    return 2;
  }
  Args a;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k.rfind("--", 0) != 0) {
      std::fprintf(stderr, "bad argument %s\n", k.c_str());
      return 2;
    }
    a.kv[k.substr(2)] = argv[i + 1];
  }
  const std::string cmd = argv[1];
  if (cmd == "synth") return cmd_synth(a);
  if (cmd == "scan") return cmd_scan(a);
  if (cmd == "validate") return cmd_validate(a);
  if (cmd == "bench") {
    BenchArgs b;
    b.input = a.get("input");
    b.json = a.get("json");
    b.symbols = a.get("symbols");
    b.label = a.get("label", "realdata");
    b.commit = a.get("commit", "unknown");
    b.os = a.get("os", "unknown");
    b.flags = a.get("flags", "unknown");
    b.note = a.get("note");
    b.reps = a.num("reps", 7);
    b.max_messages = a.num("max-messages", 0);
    b.cpu = static_cast<int>(a.num("cpu", 0)) ;
    if (!a.kv.count("cpu")) b.cpu = -1;
    b.load = a.kv.count("load") ? std::atof(a.get("load").c_str()) : -1;
    return run_bench(b);
  }
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
