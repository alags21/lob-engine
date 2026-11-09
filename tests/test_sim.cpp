#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "lob/market_sim.hpp"
#include "lob/replay_io.hpp"

using namespace lob;
using namespace lob::sim;
using SimRun = lob::sim::Run;  // plain `Run` would resolve to testing::Test::Run inside TEST bodies

namespace {

MarketConfig base_cfg() {
  MarketConfig c;
  c.seed = 1;
  c.ops = 5000;
  return c;
}

std::uint64_t count_events(const SimRun& r, EventType t) {
  std::uint64_t n = 0;
  for (const auto& e : r.events) n += e.ev.type == t;
  return n;
}
std::uint64_t count_ops(const SimRun& r, Op::Kind k) {
  std::uint64_t n = 0;
  for (const auto& o : r.ops) n += o.op.kind == k;
  return n;
}

}  // namespace

TEST(MarketSim, DeterministicPerSeed) {
  const SimRun a = simulate(base_cfg()), b = simulate(base_cfg());
  EXPECT_EQ(a.checksum, b.checksum);
  EXPECT_EQ(a.events.size(), b.events.size());
  MarketConfig other = base_cfg();
  other.seed = 2;
  EXPECT_NE(simulate(other).checksum, a.checksum);
}

// Pins the generator's output (config seed 1, 5000 ops). The generator uses only integer math and
// IEEE + - * /, so this value must be identical on every platform and compiler; CI on Linux
// checks it against the value produced on the Windows development machine.
TEST(MarketSim, ChecksumIsPlatformIndependent) {
  EXPECT_EQ(simulate(base_cfg()).checksum, 0x671fde9bc7b6f6ffULL);
}

TEST(MarketSim, TimestampsStrictlyIncreaseAndMatchArrivalRate) {
  MarketConfig c = base_cfg();
  c.ops = 20000;
  c.arrival_rate_per_sec = 200000;  // mean gap 5000 ns
  const SimRun r = simulate(c);
  for (std::size_t i = 1; i < r.ops.size(); ++i) ASSERT_LT(r.ops[i - 1].t_ns, r.ops[i].t_ns);
  const double mean_gap = static_cast<double>(r.ops.back().t_ns) / static_cast<double>(r.ops.size());
  EXPECT_GT(mean_gap, 4500.0);  // Poisson mean within 10% of the configured 5000 ns
  EXPECT_LT(mean_gap, 5500.0);
}

TEST(MarketSim, KnobsChangeTheFlow) {
  MarketConfig calm = base_cfg();
  calm.aggressiveness = 0.0;
  calm.drift_prob = 0.0;
  calm.modify_fraction = 0.0;  // a modify can nudge a quote across the spread
  EXPECT_EQ(count_events(simulate(calm), EventType::Trade), 0u);  // nothing ever crosses

  MarketConfig hot = base_cfg();
  hot.aggressiveness = 0.6;
  MarketConfig mild = base_cfg();
  mild.aggressiveness = 0.1;
  EXPECT_GT(count_events(simulate(hot), EventType::Trade), count_events(simulate(mild), EventType::Trade));

  MarketConfig many = base_cfg(), few = base_cfg();
  many.cancel_fraction = 0.5;
  few.cancel_fraction = 0.1;
  EXPECT_GT(count_ops(simulate(many), Op::Kind::Cancel), count_ops(simulate(few), Op::Kind::Cancel));
}

TEST(MarketSim, DepthDistributionChangesBookShape) {
  MarketConfig a = base_cfg(), b = base_cfg();
  a.depth_dist = DepthDist::Geometric;
  b.depth_dist = DepthDist::FatTail;
  a.depth_mean_ticks = b.depth_mean_ticks = 6;
  a.aggressiveness = b.aggressiveness = 0;
  auto span = [](const SimRun& r) {
    const auto& s = r.snapshots.back();
    return s.bids.empty() ? 0 : s.bids.front().price - s.bids.back().price;
  };
  EXPECT_GT(span(simulate(b, 10, 200)), span(simulate(a, 10, 200)));
}

TEST(MarketSim, CancelsAndModifiesTargetLiveOrders) {
  const SimRun r = simulate(base_cfg());
  EXPECT_GT(count_ops(r, Op::Kind::Cancel), 100u);
  EXPECT_GT(count_ops(r, Op::Kind::Modify), 50u);
  EXPECT_EQ(count_events(r, EventType::Rejected), 0u);  // generator never sends invalid or stale ops
  EXPECT_GT(count_events(r, EventType::Trade), 0u);
}

TEST(MarketSim, SnapshotsCoverTheRunAndBooksAreUncrossed) {
  const SimRun r = simulate(base_cfg(), 25, 10);
  ASSERT_GE(r.snapshots.size(), 2u);
  EXPECT_EQ(r.snapshots.front().op_index, 0u);
  EXPECT_EQ(r.snapshots.back().op_index, r.ops.size());
  for (const auto& s : r.snapshots) {
    EXPECT_LE(s.bids.size(), 10u);
    if (!s.bids.empty() && !s.asks.empty()) {
      EXPECT_LT(s.bids.front().price, s.asks.front().price);
    }
    for (std::size_t i = 1; i < s.bids.size(); ++i) EXPECT_GT(s.bids[i - 1].price, s.bids[i].price);
    for (std::size_t i = 1; i < s.asks.size(); ++i) EXPECT_LT(s.asks[i - 1].price, s.asks[i].price);
  }
}

TEST(ReplayIo, CsvAndJsonlRoundTripAndReplayGivesSameChecksum) {
  const SimRun r = simulate(base_cfg());
  Recorder via_csv(10, 15), via_jsonl(10, 15);
  for (const auto& t : r.ops) {
    const auto c = parse_csv_line(to_csv_line(t));
    const auto j = parse_jsonl_line(to_jsonl_line(t));
    ASSERT_TRUE(c) << to_csv_line(t);
    ASSERT_TRUE(j) << to_jsonl_line(t);
    EXPECT_EQ(c->t_ns, t.t_ns);
    EXPECT_EQ(j->t_ns, t.t_ns);
    EXPECT_EQ(to_csv_line(*c), to_csv_line(t));
    EXPECT_EQ(to_jsonl_line(*j), to_jsonl_line(t));
    via_csv.apply_op(*c);
    via_jsonl.apply_op(*j);
  }
  EXPECT_EQ(via_csv.finish(r.config).checksum, r.checksum);
  EXPECT_EQ(via_jsonl.finish(r.config).checksum, r.checksum);
}

TEST(ReplayIo, ParsersRejectMalformedLines) {
  EXPECT_FALSE(parse_csv_line(""));
  EXPECT_FALSE(parse_csv_line(csv_header()));
  EXPECT_FALSE(parse_csv_line("1,S,1,B,L,GTC,100"));
  EXPECT_FALSE(parse_csv_line("1,X,1,B,L,GTC,100,5"));
  EXPECT_FALSE(parse_csv_line("1,S,1,Q,L,GTC,100,5"));
  EXPECT_FALSE(parse_jsonl_line("{}"));
  EXPECT_FALSE(parse_jsonl_line("not json"));
  EXPECT_FALSE(parse_jsonl_line("{\"t\":1,\"op\":\"S\",\"id\":1}"));
  EXPECT_TRUE(parse_csv_line("5,C,7,,,,,"));
}

TEST(RunExport, JsonHasExpectedStructure) {
  MarketConfig c = base_cfg();
  c.ops = 300;
  const SimRun r = simulate(c, 10, 5);
  const std::string j = run_to_json(r);
  EXPECT_EQ(j.rfind("{\"format\":\"lob-run-v1\"", 0), 0u);
  for (const char* key : {"\"meta\"", "\"ops\":[", "\"events\":[", "\"snapshots\":[", "\"checksum\""})
    EXPECT_NE(j.find(key), std::string::npos) << key;
  EXPECT_EQ(run_to_js(r).rfind("window.LOB_RUN = {", 0), 0u);
  // one line per op / event / snapshot inside the arrays
  std::size_t lines = 0;
  for (char ch : j) lines += ch == '\n';
  EXPECT_GE(lines, r.ops.size() + r.events.size() + r.snapshots.size());
}
