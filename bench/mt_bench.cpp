// lob_mt_bench: producer -> queue -> matching-thread benchmark (Stage 4).
//
//   lob_mt_bench --queue KIND --workload W [--ops N] [--reps R] [--rate OPS_PER_SEC]
//                [--capacity C] [--callback on|off] [--cpu-a X --cpu-b Y] --out FILE.json
//                [--label L --commit SHA --os S --flags F --load PCT --note TXT]
//
// KIND: single-thread (no queue; the Stage 3 ceiling), mutex-book (producer applies each op under
// a std::mutex), mutex-cv-queue (bounded mutex+condvar queue), spsc (lock-free ring, spin wait),
// spsc-yield, spsc-backoff (wait-strategy comparison), spsc-unpadded (false-sharing ablation).
//
// Two measurements per repetition, always on a fresh book and queue:
//  * throughput mode: the producer pushes as fast as the queue admits (queue saturates);
//    throughput = ops / (time from first push to the consumer finishing the last op).
//  * latency mode: the producer is paced at --rate ops/s on a fixed schedule. Each op's latency is
//    (consumer completion TSC) - (INTENDED send TSC). Measuring from the intended time, not the
//    actual push time, charges producer stalls (queue full, producer behind schedule) to the
//    latency instead of hiding them (coordinated omission). The fraction of ops for which the
//    producer was already more than one period behind schedule when it reached them is reported
//    as `behind_schedule_fraction`; if it is large the configuration cannot sustain the rate and
//    the latency numbers describe overload, not service time.
// For single-thread and mutex-book there is no handoff: latency is (completion - intended).
//
// Correctness gate: before timing, every (queue, workload) is run once with a hashing event
// callback and its event+book checksum must equal the single-thread run's; otherwise exit 3.
// Event and trade counts of every timed pass are checked too.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
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

#include "lob/checksum.hpp"
#include "lob/mutex_queue.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;

// ------------------------------------------------------------------------------ clock
static inline std::uint64_t cycles() {
#ifdef LOB_HAS_TSC
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

static inline void cpu_relax() {
#ifdef LOB_HAS_TSC
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

static double calibrate_ticks_per_ns(double* overhead_ns) {
#ifdef LOB_HAS_TSC
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t a = cycles();
  while (clk::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t b = cycles();
  const double ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
  const double tpn = static_cast<double>(b - a) / ns;
#else
  const double tpn = 1.0;
#endif
  std::vector<std::uint64_t> d(10000);
  for (auto& x : d) {
    const std::uint64_t s = cycles();
    x = cycles() - s;
  }
  std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
  *overhead_ns = static_cast<double>(d[d.size() / 2]) / tpn;
  return tpn;
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

static bool invariant_tsc() {
#ifdef LOB_HAS_TSC
  int r[4] = {0};
#if defined(_MSC_VER)
  __cpuid(r, 0x80000007);
#else
  if (!__get_cpuid(0x80000007u, reinterpret_cast<unsigned*>(&r[0]), reinterpret_cast<unsigned*>(&r[1]),
                   reinterpret_cast<unsigned*>(&r[2]), reinterpret_cast<unsigned*>(&r[3])))
    return false;
#endif
  return (r[3] >> 8) & 1;  // EDX bit 8: invariant TSC
#else
  return false;
#endif
}

static bool pin_this_thread(int cpu) {
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

// Picks two logical CPUs that belong to different physical cores (skipping the first two cores,
// which tend to take interrupts). Returns a description of what was done.
static std::string pick_cores(int& a, int& b) {
  a = 0;
  b = 1;
  std::vector<int> firsts;  // lowest logical CPU of each physical core
#if defined(_WIN32)
  DWORD len = 0;
  GetLogicalProcessorInformation(nullptr, &len);
  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (len && GetLogicalProcessorInformation(info.data(), &len)) {
    for (const auto& e : info) {
      if (e.Relationship != RelationProcessorCore || e.ProcessorMask == 0) continue;
      int bit = 0;
      while (!((e.ProcessorMask >> bit) & 1)) ++bit;
      firsts.push_back(bit);
    }
  }
#elif defined(__linux__)
  for (int c = 0; c < 1024; ++c) {
    std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/thread_siblings_list");
    if (!f) break;
    std::string line;
    std::getline(f, line);
    if (std::atoi(line.c_str()) == c) firsts.push_back(c);
  }
#endif
  std::sort(firsts.begin(), firsts.end());
  if (firsts.size() >= 4) {
    a = firsts[2];
    b = firsts[3];
    return "detected " + std::to_string(firsts.size()) + " physical cores; used the first logical CPU of physical cores #2 and #3 (logical " +
           std::to_string(a) + " and " + std::to_string(b) + ")";
  }
  return "could not detect physical cores; used logical CPUs 0 and 1 (may be SMT siblings)";
}

// Estimates the TSC offset between two pinned cores with a ping-pong; offset ~ (m1 - m2) / 2 where
// m1 = min(tb - ta0), m2 = min(ta1 - tb). Also gives the minimum one-way hand-off delay.
struct TscCheck {
  double offset_ns = 0, min_one_way_ns = 0;
  int rounds = 0;
};
static TscCheck tsc_ping_pong(int cpu_a, int cpu_b, double tpn) {
  constexpr int kRounds = 20000;
  std::atomic<std::uint64_t> seq_a{0}, seq_b{0};
  std::uint64_t ta = 0, tb = 0;
  std::thread th([&] {
    pin_this_thread(cpu_b);
    for (int i = 1; i <= kRounds; ++i) {
      while (seq_a.load(std::memory_order_acquire) != static_cast<std::uint64_t>(i)) cpu_relax();
      tb = cycles();
      seq_b.store(static_cast<std::uint64_t>(i), std::memory_order_release);
    }
  });
  pin_this_thread(cpu_a);
  std::int64_t m1 = INT64_MAX, m2 = INT64_MAX;
  for (int i = 1; i <= kRounds; ++i) {
    ta = cycles();
    seq_a.store(static_cast<std::uint64_t>(i), std::memory_order_release);
    while (seq_b.load(std::memory_order_acquire) != static_cast<std::uint64_t>(i)) cpu_relax();
    const std::uint64_t ta1 = cycles();
    m1 = std::min(m1, static_cast<std::int64_t>(tb) - static_cast<std::int64_t>(ta));
    m2 = std::min(m2, static_cast<std::int64_t>(ta1) - static_cast<std::int64_t>(tb));
  }
  th.join();
  TscCheck c;
  c.rounds = kRounds;
  c.offset_ns = static_cast<double>(m1 - m2) / 2.0 / tpn;
  c.min_one_way_ns = static_cast<double>(m1 + m2) / 2.0 / tpn;
  return c;
}

// ------------------------------------------------------------------- queue adapters
enum class Wait { Spin, Yield, Backoff };

struct Waiter {
  Wait w;
  unsigned n = 0;
  void operator()() {
    switch (w) {
      case Wait::Spin: cpu_relax(); break;
      case Wait::Yield: std::this_thread::yield(); break;
      case Wait::Backoff:
        if (++n < 128) cpu_relax();
        else std::this_thread::yield();
        break;
    }
  }
};

struct Item {
  Op op;
  std::uint64_t ts;  // intended send time (TSC) in latency mode, else 0
};

template <bool Padded>
struct SpscAdapter {
  SpscQueue<Item, Padded> q;
  Wait wait;
  SpscAdapter(std::size_t cap, Wait w) : q(cap), wait(w) {}
  void push(const Item& it) {
    Waiter w{wait};
    while (!q.try_push(it)) w();
  }
  void pop(Item& it) {
    Waiter w{wait};
    while (!q.try_pop(it)) w();
  }
};

struct CvAdapter {
  MutexQueue<Item> q;
  explicit CvAdapter(std::size_t cap) : q(cap) {}
  void push(const Item& it) { q.push(it); }
  void pop(Item& it) { q.pop(it); }
};

// ------------------------------------------------------------------------------ passes
struct Counters {
  std::uint64_t events = 0, trades = 0, traded_qty = 0;
  bool operator==(const Counters&) const = default;
};

struct PassCfg {
  bool latency = false;
  double rate = 500000;  // ops/s for latency mode
  bool callback = true;
  int cpu_a = -1, cpu_b = -1;
  double tpn = 1;
};

struct PassOut {
  double seconds = 0;
  std::vector<std::uint64_t> lat;  // ticks, latency mode
  Counters counters;
  std::uint64_t hash = 0;
  double behind_fraction = 0;
};

// Builds the book with the requested event handler (hash / count / none).
struct Sink {
  Counters c;
  EventHasher hasher;
  OrderBook make(bool hash, bool count) {
    if (hash) return OrderBook([this](const Event& e) { hasher(e); });
    if (count)
      return OrderBook([this](const Event& e) {
        ++c.events;
        if (e.type == EventType::Trade) {
          ++c.trades;
          c.traded_qty += static_cast<std::uint64_t>(e.qty);
        }
      });
    return OrderBook();
  }
};

static std::uint64_t period_ticks(const PassCfg& cfg) { return static_cast<std::uint64_t>(cfg.tpn * 1e9 / cfg.rate); }

// Waits until the intended send time; returns true if the producer is already more than one period late.
static inline bool pace(std::uint64_t intended, std::uint64_t period) {
  std::uint64_t now = cycles();
  if (now > intended + period) return true;
  while (now < intended) {
    cpu_relax();
    now = cycles();
  }
  return false;
}

template <class Adapter>
static PassOut queue_pass(Adapter& ad, const std::vector<Op>& ops, const PassCfg& cfg, bool hash) {
  PassOut out;
  Sink sink;
  OrderBook book = sink.make(hash, cfg.callback);
  if (cfg.latency) out.lat.assign(ops.size(), 0);
  std::atomic<bool> go{false};
  std::chrono::steady_clock::time_point t_end;
  std::thread consumer([&] {
    pin_this_thread(cfg.cpu_b);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    Item it;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      ad.pop(it);
      apply(book, it.op);
      if (cfg.latency) out.lat[i] = cycles() - it.ts;
    }
    t_end = std::chrono::steady_clock::now();
  });
  pin_this_thread(cfg.cpu_a);
  const std::uint64_t period = cfg.latency ? period_ticks(cfg) : 0;
  std::uint64_t behind = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t base = cycles() + 2000000;  // small lead so the first op is not late
  go.store(true, std::memory_order_release);
  for (std::size_t i = 0; i < ops.size(); ++i) {
    Item it{ops[i], 0};
    if (cfg.latency) {
      const std::uint64_t intended = base + i * period;
      if (pace(intended, period)) ++behind;
      it.ts = intended;
    }
    ad.push(it);
  }
  consumer.join();
  out.seconds = std::chrono::duration<double>(t_end - t0).count();
  out.counters = sink.c;
  out.hash = hash ? digest_book(sink.hasher.h, book) : 0;
  out.behind_fraction = ops.empty() ? 0 : static_cast<double>(behind) / static_cast<double>(ops.size());
  return out;
}

// No handoff: the calling thread applies each op (optionally under a mutex).
static PassOut direct_pass(const std::vector<Op>& ops, const PassCfg& cfg, bool hash, bool use_mutex) {
  PassOut out;
  Sink sink;
  OrderBook book = sink.make(hash, cfg.callback);
  std::mutex m;
  pin_this_thread(cfg.cpu_a);
  if (cfg.latency) out.lat.assign(ops.size(), 0);
  const std::uint64_t period = cfg.latency ? period_ticks(cfg) : 0;
  std::uint64_t behind = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t base = cycles() + 2000000;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    std::uint64_t intended = 0;
    if (cfg.latency) {
      intended = base + i * period;
      if (pace(intended, period)) ++behind;
    }
    if (use_mutex) {
      std::lock_guard<std::mutex> lk(m);
      apply(book, ops[i]);
    } else {
      apply(book, ops[i]);
    }
    if (cfg.latency) out.lat[i] = cycles() - intended;
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  out.counters = sink.c;
  out.hash = hash ? digest_book(sink.hasher.h, book) : 0;
  out.behind_fraction = ops.empty() ? 0 : static_cast<double>(behind) / static_cast<double>(ops.size());
  return out;
}

static PassOut run_pass(const std::string& kind, const std::vector<Op>& ops, const PassCfg& cfg, std::size_t cap, bool hash) {
  if (kind == "single-thread") return direct_pass(ops, cfg, hash, false);
  if (kind == "mutex-book") return direct_pass(ops, cfg, hash, true);
  if (kind == "mutex-cv-queue") {
    CvAdapter ad(cap);
    return queue_pass(ad, ops, cfg, hash);
  }
  if (kind == "spsc" || kind == "spsc-yield" || kind == "spsc-backoff") {
    SpscAdapter<true> ad(cap, kind == "spsc" ? Wait::Spin : kind == "spsc-yield" ? Wait::Yield : Wait::Backoff);
    return queue_pass(ad, ops, cfg, hash);
  }
  if (kind == "spsc-unpadded") {
    SpscAdapter<false> ad(cap, Wait::Spin);
    return queue_pass(ad, ops, cfg, hash);
  }
  std::fprintf(stderr, "unknown queue kind %s\n", kind.c_str());
  std::exit(2);
}

// ------------------------------------------------------------------------------- JSON
static std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) {
      o += ' ';
      continue;
    }
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
  double cv_pct() const {
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

struct Pcts {
  double p50 = 0, p99 = 0, p999 = 0, max = 0;
};
static Pcts percentiles(std::vector<std::uint64_t>& v, double tpn) {
  Pcts p;
  if (v.empty()) return p;
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())));
    return static_cast<double>(v[i]) / tpn;
  };
  p.p50 = at(0.5);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.max = static_cast<double>(v.back()) / tpn;
  return p;
}

int main(int argc, char** argv) {
  std::string kind = "spsc", wl = "balanced", out, label, commit = "unknown", os = "unknown", flags = "unknown", note;
  std::uint64_t ops_n = 2'000'000, seed = 1, capacity = 4096;
  double rate = 500000, load = -1;
  int reps = 7, cpu_a = -1, cpu_b = -1;
  bool callback = true;
  for (int i = 1; i < argc; ++i) {
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    const std::string a = argv[i];
    if (a == "--queue") kind = val();
    else if (a == "--workload") wl = val();
    else if (a == "--ops") ops_n = std::strtoull(val(), nullptr, 10);
    else if (a == "--reps") reps = std::atoi(val());
    else if (a == "--rate") rate = std::atof(val());
    else if (a == "--capacity") capacity = std::strtoull(val(), nullptr, 10);
    else if (a == "--callback") callback = std::string(val()) != "off";
    else if (a == "--cpu-a") cpu_a = std::atoi(val());
    else if (a == "--cpu-b") cpu_b = std::atoi(val());
    else if (a == "--seed") seed = std::strtoull(val(), nullptr, 10);
    else if (a == "--out") out = val();
    else if (a == "--label") label = val();
    else if (a == "--commit") commit = val();
    else if (a == "--os") os = val();
    else if (a == "--flags") flags = val();
    else if (a == "--load") load = std::atof(val());
    else if (a == "--note") note = val();
    else {
      std::fprintf(stderr, "unknown arg %s\n", a.c_str());
      return 2;
    }
  }
  if (label.empty()) label = kind;
  if (reps < 1) reps = 1;

  std::string pin_desc;
  if (cpu_a < 0 || cpu_b < 0) pin_desc = pick_cores(cpu_a, cpu_b);
  else pin_desc = "cores given on the command line";

  WorkloadConfig cfg_w = preset(wl);
  cfg_w.seed = seed;
  cfg_w.ops = ops_n;
  Realised realised;
  const std::vector<Op> ops = generate(cfg_w, &realised);

  double overhead_ns = 0;
  const double tpn = calibrate_ticks_per_ns(&overhead_ns);
  const bool inv = invariant_tsc();
  const TscCheck tsc = tsc_ping_pong(cpu_a, cpu_b, tpn);
  std::fprintf(stderr, "[%s/%s] cpus %d,%d invariant_tsc=%d tsc_offset=%.1f ns min_one_way=%.1f ns\n", kind.c_str(),
               wl.c_str(), cpu_a, cpu_b, inv ? 1 : 0, tsc.offset_ns, tsc.min_one_way_ns);

  PassCfg pc;
  pc.callback = callback;
  pc.cpu_a = cpu_a;
  pc.cpu_b = cpu_b;
  pc.tpn = tpn;
  pc.rate = rate;

  // Reference: single-thread run with the hashing callback and the counting callback.
  PassCfg ref_cfg = pc;
  ref_cfg.callback = true;
  const PassOut ref_hash = direct_pass(ops, ref_cfg, true, false);
  const PassOut ref_count = direct_pass(ops, ref_cfg, false, false);

  // Correctness gate (untimed): the queue run must reproduce the single-thread event stream.
  const PassOut chk = run_pass(kind, ops, pc, capacity, true);
  if (chk.hash != ref_hash.hash) {
    std::fprintf(stderr, "CHECKSUM MISMATCH for %s/%s: %016llx vs single-thread %016llx\n", kind.c_str(), wl.c_str(),
                 static_cast<unsigned long long>(chk.hash), static_cast<unsigned long long>(ref_hash.hash));
    return 3;
  }

  (void)run_pass(kind, ops, pc, capacity, false);  // warmup: faults pages, warms queue and caches

  Summary tput, p50, p99, p999, pmax, behind;
  std::vector<std::uint64_t> hist(40, 0);
  for (int r = 0; r < reps; ++r) {
    PassCfg t = pc;
    t.latency = false;
    PassOut o = run_pass(kind, ops, t, capacity, false);
    if (callback && !(o.counters == ref_count.counters)) {
      std::fprintf(stderr, "EVENT COUNT MISMATCH in throughput pass (%s/%s)\n", kind.c_str(), wl.c_str());
      return 3;
    }
    tput.reps.push_back(static_cast<double>(ops.size()) / o.seconds);

    PassCfg l = pc;
    l.latency = true;
    PassOut lo = run_pass(kind, ops, l, capacity, false);
    if (callback && !(lo.counters == ref_count.counters)) {
      std::fprintf(stderr, "EVENT COUNT MISMATCH in latency pass (%s/%s)\n", kind.c_str(), wl.c_str());
      return 3;
    }
    for (auto x : lo.lat) {
      const double ns = static_cast<double>(x) / tpn;
      int b = ns < 1 ? 0 : static_cast<int>(std::log2(ns)) + 1;
      if (b >= 40) b = 39;
      ++hist[static_cast<std::size_t>(b)];
    }
    const Pcts p = percentiles(lo.lat, tpn);
    p50.reps.push_back(p.p50);
    p99.reps.push_back(p.p99);
    p999.reps.push_back(p.p999);
    pmax.reps.push_back(p.max);
    behind.reps.push_back(lo.behind_fraction);
    std::fprintf(stderr, "  rep %d: %.3f Mops/s | paced %.0f ops/s: p50 %.0f p99 %.0f p99.9 %.0f max %.0f ns, behind %.3f\n", r,
                 static_cast<double>(ops.size()) / o.seconds / 1e6, rate, p.p50, p.p99, p.p999, p.max, lo.behind_fraction);
  }

  std::ostringstream j;
  char hex[32];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(ref_hash.hash));
  j << "{\n  \"schema\": 1,\n  \"stage\": 4,\n  \"label\": " << jstr(label) << ",\n  \"commit\": " << jstr(commit) << ",\n  \"queue\": " << jstr(kind)
    << ",\n  \"environment\": {\n    \"cpu\": " << jstr(cpu_brand()) << ",\n    \"logical_cores\": " << std::thread::hardware_concurrency()
    << ",\n    \"os\": " << jstr(os) << ",\n    \"flags\": " << jstr(flags) << ",\n    \"cpu_load_percent_before_run\": " << num(load)
    << ",\n    \"note\": " << jstr(note) << ",\n    \"producer_cpu\": " << cpu_a << ",\n    \"consumer_cpu\": " << cpu_b
    << ",\n    \"pinning\": " << jstr(pin_desc)
    << ",\n    \"clock\": {\"name\": \"rdtsc (lfence-serialised); producer and consumer timestamps compared across cores\", \"ticks_per_ns\": " << num(tpn)
    << ", \"timestamp_pair_overhead_ns\": " << num(overhead_ns) << ", \"invariant_tsc_cpuid\": " << (inv ? "true" : "false")
    << ", \"cross_core_offset_ns_estimate\": " << num(tsc.offset_ns) << ", \"min_one_way_handoff_ns\": " << num(tsc.min_one_way_ns)
    << ", \"ping_pong_rounds\": " << tsc.rounds << "}\n  },\n";
  j << "  \"config\": {\"capacity\": " << capacity << ", \"event_callback\": " << (callback ? "true" : "false") << ", \"paced_rate_ops_per_sec\": " << num(rate)
    << ", \"warmup_reps\": 1, \"measured_reps\": " << reps
    << ", \"throughput_mode\": \"unpaced producer, queue saturates; ops / (first push to consumer done)\""
    << ", \"latency_mode\": \"paced producer; latency = consumer completion - intended send time (coordinated-omission safe)\"},\n";
  j << "  \"workload\": {\"name\": " << jstr(cfg_w.name) << ", \"seed\": " << cfg_w.seed << ", \"ops\": " << cfg_w.ops
    << ", \"events\": " << ref_count.counters.events << ", \"trades\": " << ref_count.counters.trades << "},\n";
  j << "  \"checksum\": " << jstr(hex) << ",\n  \"checksum_matches_single_thread\": true,\n";
  j << "  \"throughput_ops_per_sec\": " << tput.json() << ",\n  \"latency_ns\": {\"p50\": " << p50.json() << ", \"p99\": " << p99.json()
    << ", \"p99_9\": " << p999.json() << ", \"max\": " << pmax.json() << "},\n  \"behind_schedule_fraction\": " << behind.json()
    << ",\n  \"latency_histogram_log2_ns\": {\"counts\": [";
  for (std::size_t i = 0; i < hist.size(); ++i) j << (i ? ", " : "") << hist[i];
  j << "]}\n}\n";
  if (out.empty()) {
    std::fputs(j.str().c_str(), stdout);
  } else {
    std::ofstream f(out, std::ios::binary);
    f << j.str();
  }
  return 0;
}
