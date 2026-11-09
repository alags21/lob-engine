// Replays an op stream through lob::OrderBook, deterministically.
//
//   lob_replay LOGFILE                       print every emitted event, one per line (op log format)
//   lob_replay [--format oplog|csv|jsonl] [--json OUT] [--js OUT] [--snapshot-every K] [--levels L] LOGFILE
//
// Formats (docs/EVENT_FORMAT.md): `oplog` is the untimed text log; `csv` and `jsonl` carry a
// timestamp per op. The format is inferred from the extension (.csv, .jsonl) when --format is not
// given. With --json/--js the run (ops, all events, periodic book snapshots) is written in the
// visualizer's format instead of printing events. Untimed op logs get 1 us between ops.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "lob/event_log.hpp"
#include "lob/market_sim.hpp"
#include "lob/order_book.hpp"
#include "lob/replay_io.hpp"

namespace {
bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::string format, json, js, path;
  std::size_t every = 10, levels = 15;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--format") format = val();
    else if (a == "--json") json = val();
    else if (a == "--js") js = val();
    else if (a == "--snapshot-every") every = std::strtoull(val(), nullptr, 10);
    else if (a == "--levels") levels = std::strtoull(val(), nullptr, 10);
    else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    } else path = a;
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: lob_replay [--format oplog|csv|jsonl] [--json OUT] [--js OUT] LOGFILE\n");
    return 2;
  }
  if (format.empty()) format = ends_with(path, ".csv") ? "csv" : ends_with(path, ".jsonl") ? "jsonl" : "oplog";
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return 2;
  }

  const bool export_run = !json.empty() || !js.empty();
  lob::sim::Recorder rec(every, levels);
  lob::OrderBook plain([](const lob::Event& e) { std::cout << lob::to_string(e) << '\n'; });

  std::string line;
  std::size_t lineno = 0;
  std::uint64_t n = 0;
  while (std::getline(in, line)) {
    ++lineno;
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    std::optional<lob::sim::TimedOp> t;
    if (format == "csv") {
      t = lob::sim::parse_csv_line(line);
      if (!t && line.rfind("t_ns", 0) == 0) continue;  // header
    } else if (format == "jsonl") {
      t = lob::sim::parse_jsonl_line(line);
    } else {
      if (auto op = lob::parse_op(line)) t = lob::sim::TimedOp{(n + 1) * 1000, *op};
    }
    if (!t) {
      std::fprintf(stderr, "%s:%zu: malformed op: %s\n", path.c_str(), lineno, line.c_str());
      return 1;
    }
    ++n;
    if (export_run) rec.apply_op(*t);
    else lob::apply(plain, t->op);
  }
  if (export_run) {
    lob::sim::MarketConfig cfg;  // replayed runs carry no generator parameters
    cfg.ops = n;
    lob::sim::Run run = rec.finish(cfg);
    run.source = "replay";
    if (!json.empty()) std::ofstream(json, std::ios::binary) << lob::sim::run_to_json(run);
    if (!js.empty()) std::ofstream(js, std::ios::binary) << lob::sim::run_to_js(run);
    std::printf("replayed %llu ops, %zu events, %zu snapshots, checksum=%016llx\n", static_cast<unsigned long long>(n),
                run.events.size(), run.snapshots.size(), static_cast<unsigned long long>(run.checksum));
  }
  return 0;
}
