// lob_bench: closed-loop single-thread benchmark of lob::OrderBook.
//
//   lob_bench --workload passive|aggressive|balanced [--ops N] [--reps R] [--cpu C] [--seed S]
//             --out FILE.json [--label L] [--commit SHA] [--os STR] [--flags STR]
//             [--load PCT] [--note STR]
//
// Method (also written into the JSON):
//  * The op stream is generated up front (bench/workload.hpp), so generation is never timed.
//  * Closed loop: ops are issued back to back, each only after the previous returned. Reported
//    latency is therefore per-operation service time. There is no queueing delay, so coordinated
//    omission does not apply, but this also does not model an open-loop arrival process.
//  * One untimed warmup repetition, then R measured repetitions, each on a fresh book.
//  * Throughput comes from a repetition with no per-op timers (wall time of the whole loop,
//    steady_clock); latency from a second repetition that timestamps every op with the TSC.
//  * The event callback (std::function, part of the public API) only counts events and sums
//    traded quantity, so the work cannot be optimised away and its cost is small and constant.
//  * A separate untimed pass hashes every event and the final book into a checksum; it must be
//    identical across implementations for the same workload and seed.
// GCC 13 reports a false positive (stringop-overflow) once the counting operator new below is
// inlined into std::vector growth in the workload generator.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#define LOB_HAS_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#endif

#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;

// ---------------------------------------------------------------- allocation counting
// Replaces global operator new/delete with malloc/free plus a counter, so each implementation's
// allocations per op can be reported. The increment is a plain non-atomic add (single thread).
static std::uint64_t g_allocs = 0;
void* operator new(std::size_t n) {
  ++g_allocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

// ------------------------------------------------------------------------------ clock
static inline std::uint64_t cycles() {
#ifdef LOB_HAS_TSC
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

struct ClockInfo {
  std::string name;
  double ticks_per_ns = 1.0;
  double overhead_ticks = 0;
  double overhead_ns = 0;
};

static ClockInfo calibrate() {
  ClockInfo c;
#ifdef LOB_HAS_TSC
  c.name = "rdtsc (lfence-serialised), converted with steady_clock calibration";
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t a = cycles();
  while (clk::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t b = cycles();
  const double ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
  c.ticks_per_ns = static_cast<double>(b - a) / ns;
#else
  c.name = "steady_clock (ns ticks)";
  c.ticks_per_ns = 1.0;
#endif
  // Overhead of one timestamp pair, measured as the median of back-to-back reads.
  std::vector<std::uint64_t> d(10000);
  for (auto& x : d) {
    const std::uint64_t s = cycles();
    const std::uint64_t e = cycles();
    x = e - s;
  }
  std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
  c.overhead_ticks = static_cast<double>(d[d.size() / 2]);
  c.overhead_ns = c.overhead_ticks / c.ticks_per_ns;
  return c;
}

static std::string cpu_brand() {
#ifdef LOB_HAS_TSC
  int r[4] = {0};
  char buf[49] = {0};
  for (unsigned i = 0; i < 3; ++i) {
#if defined(_MSC_VER)
    __cpuid(r, static_cast<int>(0x80000002u + i));
#else
    __cpuid(0x80000002u + i, r[0], r[1], r[2], r[3]);
#endif
    std::memcpy(buf + 16 * i, r, 16);
  }
  std::string s(buf);
  const auto p = s.find_first_not_of(' ');
  return p == std::string::npos ? s : s.substr(p);
#else
  return "unknown";
#endif
}

static bool pin_thread(int cpu) {
#if defined(_WIN32)
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  return cpu >= 0 && SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpu) != 0;
#elif defined(__linux__)
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

static std::string compiler_id() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
  return "msvc " + std::to_string(_MSC_FULL_VER);
#else
  return "unknown";
#endif
}

// ------------------------------------------------------------------------------ passes
struct Counters {
  std::uint64_t events = 0, trades = 0, traded_qty = 0;
};

static OrderBook make_book(Counters& c) {
  return OrderBook([&c](const Event& e) {
    ++c.events;
    if (e.type == EventType::Trade) {
      ++c.trades;
      c.traded_qty += static_cast<std::uint64_t>(e.qty);
    }
  });
}

struct PassResult {
  double seconds = 0;
  Counters counters;
  std::uint64_t allocs = 0;
  std::size_t resting = 0;
};

static PassResult throughput_pass(const std::vector<Op>& ops) {
  PassResult r;
  OrderBook book = make_book(r.counters);
  const std::uint64_t a0 = g_allocs;
  const auto t0 = std::chrono::steady_clock::now();
  for (const Op& op : ops) apply(book, op);
  const auto t1 = std::chrono::steady_clock::now();
  r.allocs = g_allocs - a0;
  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.resting = book.order_count();
  return r;
}

static std::uint64_t fnv(std::uint64_t h, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= 1099511628211ULL;
  }
  return h;
}

static std::uint64_t checksum_pass(const std::vector<Op>& ops) {
  std::uint64_t h = 14695981039346656037ULL;
  OrderBook book([&h](const Event& e) {
    h = fnv(h, static_cast<std::uint64_t>(e.type));
    h = fnv(h, e.order_id);
    h = fnv(h, e.maker_id);
    h = fnv(h, static_cast<std::uint64_t>(e.side));
    h = fnv(h, static_cast<std::uint64_t>(e.price));
    h = fnv(h, static_cast<std::uint64_t>(e.qty));
    h = fnv(h, static_cast<std::uint64_t>(e.reject));
    h = fnv(h, static_cast<std::uint64_t>(e.cancel));
    h = fnv(h, e.lost_priority ? 1 : 0);
  });
  for (const Op& op : ops) apply(book, op);
  for (Side s : {Side::Buy, Side::Sell}) {
    for (const auto& lvl : book.levels(s)) {
      h = fnv(h, static_cast<std::uint64_t>(lvl.price));
      for (const auto& o : book.orders_at(s, lvl.price)) {
        h = fnv(h, o.id);
        h = fnv(h, static_cast<std::uint64_t>(o.qty));
      }
    }
  }
  return h;
}

struct LatencyPass {
  std::vector<std::uint64_t> all;                 // ticks, all ops
  std::vector<std::uint64_t> by_kind[3];          // submit, cancel, modify
};

static LatencyPass latency_pass(const std::vector<Op>& ops) {
  LatencyPass lp;
  lp.all.resize(ops.size());
  Counters c;
  OrderBook book = make_book(c);
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const Op& op = ops[i];
    const std::uint64_t s = cycles();
    apply(book, op);
    const std::uint64_t e = cycles();
    lp.all[i] = e - s;
  }
  for (std::size_t i = 0; i < ops.size(); ++i)
    lp.by_kind[static_cast<int>(ops[i].kind)].push_back(lp.all[i]);
  return lp;
}

struct Pcts {
  double p50, p99, p999, max, mean;
};

static Pcts percentiles(std::vector<std::uint64_t>& v, double ticks_per_ns) {
  Pcts p{0, 0, 0, 0, 0};
  if (v.empty()) return p;
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())));
    return static_cast<double>(v[i]) / ticks_per_ns;
  };
  p.p50 = at(0.5);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.max = static_cast<double>(v.back()) / ticks_per_ns;
  double sum = 0;
  for (auto x : v) sum += static_cast<double>(x);
  p.mean = sum / static_cast<double>(v.size()) / ticks_per_ns;
  return p;
}

// ------------------------------------------------------------------------------- JSON
static std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) { o += ' '; continue; }
    o += c;
  }
  return o + "\"";
}

static std::string num(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}

struct Summary {
  std::vector<double> reps;
  double median() const {
    auto v = reps;
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : (v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]));
  }
  double min() const { return *std::min_element(reps.begin(), reps.end()); }
  double max() const { return *std::max_element(reps.begin(), reps.end()); }
  double cv_pct() const {  // coefficient of variation, population stddev / mean
    double m = 0;
    for (double x : reps) m += x;
    m /= static_cast<double>(reps.size());
    double s = 0;
    for (double x : reps) s += (x - m) * (x - m);
    s = std::sqrt(s / static_cast<double>(reps.size()));
    return m > 0 ? 100.0 * s / m : 0;
  }
  std::string json() const {
    std::ostringstream o;
    o << "{\"reps\": [";
    for (std::size_t i = 0; i < reps.size(); ++i) o << (i ? ", " : "") << num(reps[i]);
    o << "], \"median\": " << num(median()) << ", \"min\": " << num(min()) << ", \"max\": " << num(max())
      << ", \"cv_pct\": " << num(cv_pct()) << "}";
    return o.str();
  }
};

int main(int argc, char** argv) {
  std::string wl = "passive", out, label = "unlabelled", commit = "unknown", os = "unknown",
              flags = "unknown", note;
  std::uint64_t ops_n = 2'000'000, seed = 1;
  int reps = 7, cpu = -1;
  double load = -1;
  for (int i = 1; i < argc; ++i) {
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); std::exit(2); }
      return argv[++i];
    };
    const std::string a = argv[i];
    if (a == "--workload") wl = val();
    else if (a == "--ops") ops_n = std::strtoull(val(), nullptr, 10);
    else if (a == "--reps") reps = std::atoi(val());
    else if (a == "--cpu") cpu = std::atoi(val());
    else if (a == "--seed") seed = std::strtoull(val(), nullptr, 10);
    else if (a == "--out") out = val();
    else if (a == "--label") label = val();
    else if (a == "--commit") commit = val();
    else if (a == "--os") os = val();
    else if (a == "--flags") flags = val();
    else if (a == "--load") load = std::atof(val());
    else if (a == "--note") note = val();
    else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
  }
  if (reps < 1) reps = 1;

  WorkloadConfig cfg = preset(wl);
  cfg.seed = seed;
  cfg.ops = ops_n;
  Realised realised;
  const std::vector<Op> ops = generate(cfg, &realised);

  const bool pinned = pin_thread(cpu);
  const ClockInfo clk = calibrate();
  std::fprintf(stderr, "[%s/%s] ops=%llu reps=%d pinned=%d tsc=%.3f GHz overhead=%.1f ns\n",
               label.c_str(), wl.c_str(), static_cast<unsigned long long>(ops_n), reps, pinned ? 1 : 0,
               clk.ticks_per_ns, clk.overhead_ns);

  const std::uint64_t sum = checksum_pass(ops);
  (void)throughput_pass(ops);  // warmup (untimed): faults in pages, warms caches and predictors

  Summary tput, p50, p99, p999, pmax, pmean;
  Summary kp50[3], kp99[3];
  PassResult last;
  std::vector<std::uint64_t> hist(40, 0);  // log2(ns) buckets, aggregated over all reps
  for (int r = 0; r < reps; ++r) {
    last = throughput_pass(ops);
    tput.reps.push_back(static_cast<double>(ops.size()) / last.seconds);
    LatencyPass lp = latency_pass(ops);
    for (auto t : lp.all) {
      const double ns = static_cast<double>(t) / clk.ticks_per_ns;
      int b = ns < 1 ? 0 : static_cast<int>(std::log2(ns)) + 1;
      if (b >= 40) b = 39;
      ++hist[static_cast<std::size_t>(b)];
    }
    const Pcts a = percentiles(lp.all, clk.ticks_per_ns);
    p50.reps.push_back(a.p50);
    p99.reps.push_back(a.p99);
    p999.reps.push_back(a.p999);
    pmax.reps.push_back(a.max);
    pmean.reps.push_back(a.mean);
    for (int k = 0; k < 3; ++k) {
      const Pcts b = percentiles(lp.by_kind[k], clk.ticks_per_ns);
      kp50[k].reps.push_back(b.p50);
      kp99[k].reps.push_back(b.p99);
    }
    std::fprintf(stderr, "  rep %d: %.3f Mops/s  p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  max %.0f ns\n", r,
                 static_cast<double>(ops.size()) / last.seconds / 1e6, a.p50, a.p99, a.p999, a.max);
  }

  std::ostringstream j;
  j << "{\n  \"schema\": 1,\n  \"label\": " << jstr(label) << ",\n  \"commit\": " << jstr(commit)
    << ",\n  \"environment\": {\n    \"cpu\": " << jstr(cpu_brand())
    << ",\n    \"logical_cores\": " << std::thread::hardware_concurrency()
    << ",\n    \"os\": " << jstr(os) << ",\n    \"compiler\": " << jstr(compiler_id())
    << ",\n    \"flags\": " << jstr(flags) << ",\n    \"pinned_cpu\": " << (pinned ? cpu : -1)
    << ",\n    \"cpu_load_percent_before_run\": " << num(load)
    << ",\n    \"note\": " << jstr(note)
    << ",\n    \"clock\": {\"name\": " << jstr(clk.name) << ", \"ticks_per_ns\": " << num(clk.ticks_per_ns)
    << ", \"timestamp_pair_overhead_ns\": " << num(clk.overhead_ns) << "}\n  },\n";
  j << "  \"method\": {\"model\": \"closed loop, single thread; per-op service time, no queueing\","
    << " \"warmup_reps\": 1, \"measured_reps\": " << reps
    << ", \"throughput\": \"wall time of whole loop, no per-op timers\","
    << " \"latency\": \"per-op TSC timestamps, timer overhead not subtracted\"},\n";
  j << "  \"workload\": {\"name\": " << jstr(cfg.name) << ", \"seed\": " << cfg.seed << ", \"ops\": " << cfg.ops
    << ", \"submit_pct\": " << cfg.submit_pct << ", \"cancel_pct\": " << cfg.cancel_pct
    << ", \"modify_pct\": " << cfg.modify_pct << ", \"aggressive_pct\": " << cfg.aggressive_pct
    << ", \"depth_mean\": " << cfg.depth_mean << ", \"book_target\": " << cfg.book_target
    << ", \"realised\": {\"submits\": " << realised.submits << ", \"cancels\": " << realised.cancels
    << ", \"modifies\": " << realised.modifies << "}, \"events\": " << last.counters.events
    << ", \"trades\": " << last.counters.trades << ", \"final_resting_orders\": " << last.resting << "},\n";
  char hex[32];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(sum));
  j << "  \"checksum\": " << jstr(hex) << ",\n";
  j << "  \"allocations_per_run\": " << last.allocs << ",\n  \"allocations_per_op\": "
    << num(static_cast<double>(last.allocs) / static_cast<double>(ops.size())) << ",\n";
  j << "  \"throughput_ops_per_sec\": " << tput.json() << ",\n";
  j << "  \"latency_ns\": {\n    \"p50\": " << p50.json() << ",\n    \"p99\": " << p99.json()
    << ",\n    \"p99_9\": " << p999.json() << ",\n    \"max\": " << pmax.json() << ",\n    \"mean\": " << pmean.json()
    << ",\n    \"by_kind\": {";
  const char* names[3] = {"submit", "cancel", "modify"};
  for (int k = 0; k < 3; ++k)
    j << (k ? ", " : "") << "\"" << names[k] << "\": {\"p50\": " << kp50[k].json() << ", \"p99\": " << kp99[k].json() << "}";
  j << "}\n  },\n  \"latency_histogram_log2_ns\": {\"note\": \"bucket b counts ops with 2^(b-1) <= ns < 2^b (bucket 0: <1 ns), all measured reps\", \"counts\": [";
  for (std::size_t i = 0; i < hist.size(); ++i) j << (i ? ", " : "") << hist[i];
  j << "]}\n}\n";

  if (out.empty()) {
    std::fputs(j.str().c_str(), stdout);
  } else {
    std::ofstream f(out, std::ios::binary);
    f << j.str();
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
  }
  return 0;
}
