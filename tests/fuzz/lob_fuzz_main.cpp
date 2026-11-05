// Command-line differential fuzzer.
//   lob_fuzz --seed S --ops N [--runs K] [--check-every M] [--record DIR]
// Full book-state comparison + invariant check run every M ops (default 16); events and statuses
// are compared on every op.
// Runs K independent streams with seeds S, S+1, ..., S+K-1, each N ops long. Every seed is
// printed before it runs, so any failure is reproducible with --seed <printed> --ops N.
// On a mismatch, writes the failing op prefix to DIR/fuzz-fail-seed<S>.log (default: cwd).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fuzz.hpp"

int main(int argc, char** argv) {
  std::uint64_t seed = 1, ops = 1000000, runs = 1, check_every = 16;
  std::string dir = ".";
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--seed")) seed = std::strtoull(need("--seed"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--ops")) ops = std::strtoull(need("--ops"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--runs")) runs = std::strtoull(need("--runs"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--check-every"))
      check_every = std::strtoull(need("--check-every"), nullptr, 10);
    else if (!std::strcmp(argv[i], "--record")) dir = need("--record");
    else {
      std::fprintf(stderr, "usage: lob_fuzz --seed S --ops N [--runs K] [--record DIR]\n");
      return 2;
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t total_ops = 0, total_events = 0, total_trades = 0;
  std::size_t max_orders = 0;
  for (std::uint64_t k = 0; k < runs; ++k) {
    const std::uint64_t s = seed + k;
    std::printf("seed=%llu ops=%llu\n", static_cast<unsigned long long>(s),
                static_cast<unsigned long long>(ops));
    std::fflush(stdout);
    const auto r = lob::fuzz::run_fuzz({s, ops, check_every ? check_every : 1});
    if (!r.ok) {
      std::printf("%s", r.message.c_str());
      const std::string path = dir + "/fuzz-fail-seed" + std::to_string(s) + ".log";
      if (lob::fuzz::write_op_log(path, r.failing_prefix, s))
        std::printf("op log written to %s\n", path.c_str());
      return 1;
    }
    total_ops += r.ops;
    total_events += r.events;
    total_trades += r.trades;
    max_orders = std::max(max_orders, r.max_resting_orders);
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("PASS runs=%llu first_seed=%llu ops=%llu events=%llu trades=%llu max_resting=%zu "
              "seconds=%.1f\n",
              static_cast<unsigned long long>(runs), static_cast<unsigned long long>(seed),
              static_cast<unsigned long long>(total_ops),
              static_cast<unsigned long long>(total_events),
              static_cast<unsigned long long>(total_trades), max_orders, secs);
  return 0;
}
