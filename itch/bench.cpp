#define _CRT_SECURE_NO_WARNINGS 1
#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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

#include "itch_validate.hpp"
#include "lob/checksum.hpp"
#include "lob/itch.hpp"
#include "tool_io.hpp"

namespace lob::itch {

namespace {

inline std::uint64_t cycles() {
#ifdef LOB_HAS_TSC
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double calibrate(double* overhead_ns) {
  double tpn = 1.0;
#ifdef LOB_HAS_TSC
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t a = cycles();
  while (clk::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t b = cycles();
  tpn = static_cast<double>(b - a) / std::chrono::duration<double, std::nano>(clk::now() - t0).count();
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

std::string cpu_brand() {
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

bool pin(int cpu) {
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

struct BenchOp {
  std::uint32_t book = 0;
  std::uint8_t reset = 0;  // 1: discard and recreate the book before the next op
  lob::Op op;
};

struct Counters {
  std::uint64_t events = 0, trades = 0;
  bool operator==(const Counters&) const = default;
};

struct Replay {
  std::vector<std::unique_ptr<OrderBook>> books;
  Counters c;
  explicit Replay(std::size_t n) {
    books.resize(n);
    for (std::size_t i = 0; i < n; ++i) fresh(i);
  }
  void fresh(std::size_t i) {
    books[i] = std::make_unique<OrderBook>([this](const Event& e) {
      ++c.events;
      if (e.type == EventType::Trade) ++c.trades;
    });
  }
  void apply_one(const BenchOp& b) {
    if (b.reset) {
      fresh(b.book);
      return;
    }
    lob::apply(*books[b.book], b.op);
  }
};

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
    char b[64];
    std::ostringstream o;
    o << "{\"reps\":[";
    for (std::size_t i = 0; i < reps.size(); ++i) {
      std::snprintf(b, sizeof b, "%.6g", reps[i]);
      o << (i ? "," : "") << b;
    }
    o << "]";
    std::snprintf(b, sizeof b, "%.6g", median());
    o << ",\"median\":" << b;
    std::snprintf(b, sizeof b, "%.6g", min());
    o << ",\"min\":" << b;
    std::snprintf(b, sizeof b, "%.6g", max());
    o << ",\"max\":" << b;
    std::snprintf(b, sizeof b, "%.6g", cv_pct());
    o << ",\"cv_pct\":" << b << "}";
    return o.str();
  }
};

struct Pcts {
  double p50 = 0, p99 = 0, p999 = 0, max = 0;
};
Pcts percentiles(std::vector<std::uint64_t>& v, double tpn) {
  Pcts p;
  if (v.empty()) return p;
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {
    return static_cast<double>(v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]) / tpn;
  };
  p.p50 = at(0.5);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.max = static_cast<double>(v.back()) / tpn;
  return p;
}

enum Cat { kAdd = 0, kCancel, kModify, kExec, kCats };
const char* kCatNames[kCats] = {"add_limit_gtc", "cancel", "partial_cancel_modify", "execution_marketable_ioc"};
Cat cat_of(const BenchOp& b) {
  if (b.op.kind == lob::Op::Kind::Cancel) return kCancel;
  if (b.op.kind == lob::Op::Kind::Modify) return kModify;
  return b.op.tif == TimeInForce::IOC ? kExec : kAdd;
}

std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) o += c;
  }
  return o;
}

}  // namespace

int run_bench(const BenchArgs& a) {
  std::set<std::string> want;
  {
    std::stringstream ss(a.symbols);
    std::string tok;
    while (std::getline(ss, tok, ','))
      if (!tok.empty()) want.insert(tok);
  }
  if (want.empty()) {
    std::fprintf(stderr, "bench: --symbols is required\n");
    return 2;
  }

  // ---- phase 1: decode (decompress + parse + validate bookkeeping + engine), recording the op stream
  std::vector<BenchOp> ops;
  ops.reserve(1u << 24);
  std::map<std::uint16_t, std::uint32_t> book_of_locate;
  std::vector<std::uint16_t> locate_of_book;
  ValidateConfig cfg;
  cfg.only_symbols = want;
  cfg.op_sink = [&](std::uint16_t loc, const lob::Op& op) {
    auto it = book_of_locate.find(loc);
    if (it == book_of_locate.end()) {
      it = book_of_locate.emplace(loc, static_cast<std::uint32_t>(locate_of_book.size())).first;
      locate_of_book.push_back(loc);
    }
    BenchOp b;
    b.book = it->second;
    b.op = op;
    ops.push_back(b);
  };
  cfg.reset_sink = [&](std::uint16_t loc) {
    auto it = book_of_locate.find(loc);
    if (it == book_of_locate.end()) return;
    BenchOp b;
    b.book = it->second;
    b.reset = 1;
    ops.push_back(b);
  };
  Validator v(cfg);
  tool::FileSource src;
  if (!src.open(a.input)) {
    std::fprintf(stderr, "cannot open input\n");
    return 2;
  }
  FrameReader<tool::FileSource> fr(src);
  const std::uint8_t* body;
  std::size_t len;
  Message m;
  std::uint64_t frames = 0;
  const auto d0 = std::chrono::steady_clock::now();
  while (fr.next(body, len)) {
    ++frames;
    if (parse(body, len, m) != ParseStatus::Ok) continue;
    v.on_message(m);
    if (a.max_messages && frames >= a.max_messages) break;
    if (frames % 50000000 == 0) std::fprintf(stderr, "bench decode: %llu frames, %zu ops recorded\n", static_cast<unsigned long long>(frames), ops.size());
  }
  const double decode_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - d0).count();
  std::fprintf(stderr, "decoded %llu frames in %.1f s; %zu ops recorded for %zu books\n", static_cast<unsigned long long>(frames), decode_s, ops.size(),
               locate_of_book.size());

  // ---- phase 2: replay the recorded stream through fresh books
  const bool pinned = pin(a.cpu);
  double overhead_ns = 0;
  const double tpn = calibrate(&overhead_ns);
  std::fprintf(stderr, "replay: %zu ops, pinned=%d cpu=%d tsc=%.3f GHz overhead=%.1f ns\n", ops.size(), pinned ? 1 : 0, a.cpu, tpn, overhead_ns);

  auto throughput_pass = [&](Counters* out_c, std::vector<std::unique_ptr<OrderBook>>* keep) {
    Replay r(locate_of_book.size());
    const auto t0 = std::chrono::steady_clock::now();
    for (const BenchOp& b : ops) r.apply_one(b);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (out_c) *out_c = r.c;
    if (keep) *keep = std::move(r.books);
    return secs;
  };

  Counters counters;
  std::vector<std::unique_ptr<OrderBook>> final_books;
  (void)throughput_pass(&counters, &final_books);  // warmup; also provides the final books for the digest check

  // Determinism check: replayed final books must equal the books the validation run ended with.
  // (Validator::book_digest looks symbols up by name; the recorder keyed books by locate, so map
  // through the stream's directory messages.)
  std::uint64_t digests_checked = 0, digests_equal = 0;
  {
    // Re-derive name -> book index: the validator holds one Symbol per locate, in `symbols()` order
    // by message count; look up each wanted name and compare against any replayed book with the same digest.
    std::set<std::uint64_t> replay_digests;
    for (const auto& b : final_books) replay_digests.insert(lob::digest_book(14695981039346656037ULL, *b));
    for (const auto& name : want) {
      const std::uint64_t d = v.book_digest(name);
      if (d == 0) continue;
      ++digests_checked;
      if (replay_digests.count(d)) ++digests_equal;
    }
  }

  Summary tput, p50, p99, p999, pmax;
  Summary cp50[kCats], cp99[kCats];
  std::uint64_t cat_count[kCats] = {};
  for (const auto& b : ops)
    if (!b.reset) ++cat_count[cat_of(b)];
  for (std::uint64_t r = 0; r < a.reps; ++r) {
    const double secs = throughput_pass(nullptr, nullptr);
    tput.reps.push_back(static_cast<double>(ops.size()) / secs);

    Replay rp(locate_of_book.size());
    std::vector<std::uint64_t> lat(ops.size());
    for (std::size_t i = 0; i < ops.size(); ++i) {
      const std::uint64_t t0 = cycles();
      rp.apply_one(ops[i]);
      lat[i] = cycles() - t0;
    }
    std::vector<std::uint64_t> by[kCats];
    for (std::size_t i = 0; i < ops.size(); ++i)
      if (!ops[i].reset) by[cat_of(ops[i])].push_back(lat[i]);
    const Pcts all = percentiles(lat, tpn);
    p50.reps.push_back(all.p50);
    p99.reps.push_back(all.p99);
    p999.reps.push_back(all.p999);
    pmax.reps.push_back(all.max);
    for (int c = 0; c < kCats; ++c) {
      const Pcts pc = percentiles(by[c], tpn);
      cp50[c].reps.push_back(pc.p50);
      cp99[c].reps.push_back(pc.p99);
    }
    std::fprintf(stderr, "  rep %llu: %.3f Mops/s p50 %.0f ns p99 %.0f ns p99.9 %.0f ns max %.0f ns\n", static_cast<unsigned long long>(r),
                 static_cast<double>(ops.size()) / secs / 1e6, all.p50, all.p99, all.p999, all.max);
  }

  std::uint64_t resting = 0;
  for (const auto& b : final_books) resting += b->order_count();

  std::ostringstream j;
  char buf[64];
  j << "{\"schema\":1,\"stage\":7,\"label\":\"" << esc(a.label) << "\",\"commit\":\"" << esc(a.commit) << "\",\"input_file_name\":\"07302019.NASDAQ_ITCH50.gz\","
    << "\"environment\":{\"cpu\":\"" << esc(cpu_brand()) << "\",\"logical_cores\":" << std::thread::hardware_concurrency() << ",\"os\":\"" << esc(a.os)
    << "\",\"flags\":\"" << esc(a.flags) << "\",\"pinned_cpu\":" << (pinned ? a.cpu : -1) << ",\"cpu_load_percent_before_run\":" << a.load << ",\"note\":\""
    << esc(a.note) << "\",\"clock\":{\"name\":\"rdtsc (lfence-serialised)\",\"ticks_per_ns\":";
  std::snprintf(buf, sizeof buf, "%.6g", tpn);
  j << buf << ",\"timestamp_pair_overhead_ns\":";
  std::snprintf(buf, sizeof buf, "%.6g", overhead_ns);
  j << buf << "}},\"method\":{\"model\":\"closed loop, single thread, in-memory pre-decoded op stream\","
    << "\"separation\":\"decompression, ITCH parsing and validation bookkeeping happen once in phase 1 (decode_seconds) and are NOT in the timed replay\","
    << "\"warmup_reps\":1,\"measured_reps\":" << a.reps
    << ",\"throughput\":\"wall time of the whole replay loop, no per-op timers\",\"latency\":\"per-op TSC timestamps, timer overhead not subtracted\","
    << "\"event_callback\":\"counts events and trades\"},";
  j << "\"workload\":{\"symbols\":[";
  bool first = true;
  for (const auto& n : want) {
    j << (first ? "" : ",") << "\"" << esc(n) << "\"";
    first = false;
  }
  j << "],\"books\":" << locate_of_book.size() << ",\"itch_frames_read\":" << frames << ",\"ops_replayed\":" << ops.size() << ",\"ops_by_category\":{";
  for (int c = 0; c < kCats; ++c) j << (c ? "," : "") << "\"" << kCatNames[c] << "\":" << cat_count[c];
  j << "},\"events\":" << counters.events << ",\"trades\":" << counters.trades << ",\"resting_orders_at_end\":" << resting
    << ",\"decode_seconds_including_decompression\":";
  std::snprintf(buf, sizeof buf, "%.6g", decode_s);
  j << buf << ",\"final_book_digests_checked\":" << digests_checked << ",\"final_book_digests_equal_to_validation_run\":" << digests_equal << "},";
  j << "\"throughput_ops_per_sec\":" << tput.json() << ",\"latency_ns\":{\"p50\":" << p50.json() << ",\"p99\":" << p99.json() << ",\"p99_9\":" << p999.json()
    << ",\"max\":" << pmax.json() << ",\"by_category\":{";
  for (int c = 0; c < kCats; ++c)
    j << (c ? "," : "") << "\"" << kCatNames[c] << "\":{\"p50\":" << cp50[c].json() << ",\"p99\":" << cp99[c].json() << "}";
  j << "}}}";
  std::ofstream f(a.json, std::ios::binary);
  f << j.str();
  return f ? 0 : 1;
}

}  // namespace lob::itch
