// lob_sim: generate a seeded synthetic market run and export it.
//
//   lob_sim [--seed S] [--ops N] [--rate OPS_PER_SEC] [--cancel F] [--modify F] [--aggr F]
//           [--depth-dist geometric|uniform|fat_tail] [--depth-mean TICKS] [--mid TICKS] [--drift F]
//           [--max-qty Q] [--book-target N] [--snapshot-every K] [--levels L]
//           [--json FILE] [--js FILE] [--csv FILE] [--jsonl FILE]
//
// --json/--js write the visualizer run format; --csv/--jsonl write only the input ops (replayable
// with lob_replay). A summary is printed to stdout.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "lob/market_sim.hpp"
#include "lob/replay_io.hpp"

using namespace lob;
using namespace lob::sim;

int main(int argc, char** argv) {
  MarketConfig cfg;
  std::size_t every = 10, levels = 15;
  std::string json, js, csv, jsonl;
  for (int i = 1; i < argc; ++i) {
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    const std::string a = argv[i];
    if (a == "--seed") cfg.seed = std::strtoull(val(), nullptr, 10);
    else if (a == "--ops") cfg.ops = std::strtoull(val(), nullptr, 10);
    else if (a == "--rate") cfg.arrival_rate_per_sec = std::atof(val());
    else if (a == "--cancel") cfg.cancel_fraction = std::atof(val());
    else if (a == "--modify") cfg.modify_fraction = std::atof(val());
    else if (a == "--aggr") cfg.aggressiveness = std::atof(val());
    else if (a == "--depth-dist") {
      if (!parse_depth_dist(val(), cfg.depth_dist)) {
        std::fprintf(stderr, "unknown depth distribution\n");
        return 2;
      }
    } else if (a == "--depth-mean") cfg.depth_mean_ticks = std::atoi(val());
    else if (a == "--mid") cfg.initial_mid = std::strtoll(val(), nullptr, 10);
    else if (a == "--drift") cfg.drift_prob = std::atof(val());
    else if (a == "--max-qty") cfg.max_qty = std::atoi(val());
    else if (a == "--book-target") cfg.book_target = std::strtoull(val(), nullptr, 10);
    else if (a == "--snapshot-every") every = std::strtoull(val(), nullptr, 10);
    else if (a == "--levels") levels = std::strtoull(val(), nullptr, 10);
    else if (a == "--json") json = val();
    else if (a == "--js") js = val();
    else if (a == "--csv") csv = val();
    else if (a == "--jsonl") jsonl = val();
    else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (cfg.arrival_rate_per_sec <= 0) {
    std::fprintf(stderr, "--rate must be positive\n");
    return 2;
  }

  const Run run = simulate(cfg, every, levels);
  auto write = [](const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
    if (!f) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      std::exit(1);
    }
  };
  if (!json.empty()) write(json, run_to_json(run));
  if (!js.empty()) write(js, run_to_js(run));
  if (!csv.empty()) {
    std::string s = csv_header() + "\n";
    for (const auto& t : run.ops) s += to_csv_line(t) + "\n";
    write(csv, s);
  }
  if (!jsonl.empty()) {
    std::string s;
    for (const auto& t : run.ops) s += to_jsonl_line(t) + "\n";
    write(jsonl, s);
  }
  std::uint64_t trades = 0;
  for (const auto& e : run.events) trades += e.ev.type == EventType::Trade;
  std::printf("seed=%llu ops=%zu events=%zu trades=%llu snapshots=%zu checksum=%016llx\n",
              static_cast<unsigned long long>(cfg.seed), run.ops.size(), run.events.size(),
              static_cast<unsigned long long>(trades), run.snapshots.size(), static_cast<unsigned long long>(run.checksum));
  return 0;
}
