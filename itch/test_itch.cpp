#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "itch_synth.hpp"
#include "itch_validate.hpp"
#include "lob/itch.hpp"

using namespace lob::itch;

namespace {

// Hand-written big-endian byte builder, deliberately independent of lob::itch::append_framed.
struct Bytes {
  std::vector<std::uint8_t> v;
  Bytes& u(std::uint64_t x, int n) {
    for (int i = n - 1; i >= 0; --i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xff));
    return *this;
  }
  Bytes& c(char ch) { return u(static_cast<unsigned char>(ch), 1); }
  Bytes& s8(const char* s) {
    std::size_t n = std::strlen(s);
    for (std::size_t i = 0; i < 8; ++i) c(i < n ? s[i] : ' ');
    return *this;
  }
  Bytes& header(char type, unsigned locate, std::uint64_t ts) { return c(type).u(locate, 2).u(7, 2).u(ts, 6); }
};

Message must_parse(const Bytes& b) {
  Message m;
  EXPECT_EQ(parse(b.v.data(), b.v.size(), m), ParseStatus::Ok);
  return m;
}

}  // namespace

TEST(ItchParse, AddOrderFromHandBuiltBytes) {
  Bytes b;
  b.header('A', 1, 0x1234).u(5, 8).c('B').u(100, 4).s8("AAPL").u(1000000, 4);
  ASSERT_EQ(b.v.size(), 36u);
  const Message m = must_parse(b);
  EXPECT_EQ(m.type, 'A');
  EXPECT_EQ(m.locate, 1);
  EXPECT_EQ(m.ts_ns, 0x1234u);
  EXPECT_EQ(m.ref, 5u);
  EXPECT_EQ(m.side, 'B');
  EXPECT_EQ(m.qty, 100u);
  EXPECT_STREQ(m.stock, "AAPL");
  EXPECT_EQ(m.price, 1000000);
}

TEST(ItchParse, SixByteTimestampUsesAllBytes) {
  Bytes b;
  b.header('D', 9, 0x0000'0102'0304'0506ULL).u(77, 8);
  const Message m = must_parse(b);
  EXPECT_EQ(m.ts_ns, 0x0102'0304'0506ULL);
  EXPECT_EQ(m.ref, 77u);
  EXPECT_EQ(m.locate, 9);
}

TEST(ItchParse, ExecutedExecutedWithPriceCancelDeleteReplace) {
  {
    Bytes b;
    b.header('E', 2, 10).u(5, 8).u(40, 4).u(999, 8);
    const Message m = must_parse(b);
    EXPECT_EQ(m.ref, 5u);
    EXPECT_EQ(m.qty, 40u);
    EXPECT_EQ(m.match, 999u);
  }
  {
    Bytes b;
    b.header('C', 2, 10).u(5, 8).u(40, 4).u(999, 8).c('N').u(1234567, 4);
    ASSERT_EQ(b.v.size(), 36u);
    const Message m = must_parse(b);
    EXPECT_EQ(m.qty, 40u);
    EXPECT_FALSE(m.printable);
    EXPECT_EQ(m.price, 1234567);
  }
  {
    Bytes b;
    b.header('X', 2, 10).u(5, 8).u(25, 4);
    const Message m = must_parse(b);
    EXPECT_EQ(m.ref, 5u);
    EXPECT_EQ(m.qty, 25u);
  }
  {
    Bytes b;
    b.header('U', 2, 10).u(5, 8).u(6, 8).u(300, 4).u(1000100, 4);
    ASSERT_EQ(b.v.size(), 35u);
    const Message m = must_parse(b);
    EXPECT_EQ(m.ref, 5u);
    EXPECT_EQ(m.new_ref, 6u);
    EXPECT_EQ(m.qty, 300u);
    EXPECT_EQ(m.price, 1000100);
  }
}

TEST(ItchParse, SystemHaltTradeAndCross) {
  {
    Bytes b;
    b.header('S', 0, 1).c('Q');
    EXPECT_EQ(must_parse(b).code, 'Q');
  }
  {
    Bytes b;
    b.header('H', 4, 1).s8("MSFT").c('H').c(' ').u(0x20202020, 4);
    ASSERT_EQ(b.v.size(), 25u);
    const Message m = must_parse(b);
    EXPECT_EQ(m.code, 'H');
    EXPECT_STREQ(m.stock, "MSFT");
  }
  {
    Bytes b;
    b.header('P', 4, 1).u(0, 8).c('S').u(500, 4).s8("MSFT").u(1500000, 4).u(31337, 8);
    ASSERT_EQ(b.v.size(), 44u);
    const Message m = must_parse(b);
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(m.qty, 500u);
    EXPECT_EQ(m.match, 31337u);
  }
  {
    Bytes b;
    b.header('Q', 4, 1).u(1234567890123ULL, 8).s8("MSFT").u(1500000, 4).u(55, 8).c('O');
    ASSERT_EQ(b.v.size(), 40u);
    const Message m = must_parse(b);
    EXPECT_EQ(m.qty, 1234567890123ULL);
    EXPECT_EQ(m.code, 'O');
  }
}

TEST(ItchParse, WrongLengthIsMalformedAndUnknownTypeIsSkipped) {
  Bytes b;
  b.header('D', 1, 1).u(1, 8).c('x');  // one byte too long
  Message m;
  EXPECT_EQ(parse(b.v.data(), b.v.size(), m), ParseStatus::Malformed);
  Bytes z;
  z.c('Z').u(0, 10);
  EXPECT_EQ(parse(z.v.data(), z.v.size(), m), ParseStatus::Unknown);
  EXPECT_EQ(parse(z.v.data(), 0, m), ParseStatus::Malformed);
}

TEST(ItchParse, EncoderRoundTripsEverySupportedType) {
  std::vector<Message> msgs;
  auto mk = [&](char t) {
    Message m;
    m.type = t;
    m.locate = 321;
    m.ts_ns = 34'200'123'456'789ULL;
    return m;
  };
  Message s = mk('S'); s.code = 'M'; msgs.push_back(s);
  Message r = mk('R'); std::snprintf(r.stock, sizeof r.stock, "ZVZZT"); msgs.push_back(r);
  Message h = mk('H'); h.code = 'T'; std::snprintf(h.stock, sizeof h.stock, "ZVZZT"); msgs.push_back(h);
  for (char t : {'A', 'F'}) {
    Message a = mk(t); a.ref = 0x0102030405060708ULL; a.side = 'S'; a.qty = 4294967295ULL; a.price = 2147483647; std::snprintf(a.stock, sizeof a.stock, "ABCDEFGH");
    msgs.push_back(a);
  }
  Message e = mk('E'); e.ref = 11; e.qty = 22; e.match = 33; msgs.push_back(e);
  Message c = mk('C'); c.ref = 11; c.qty = 22; c.match = 33; c.printable = false; c.price = 4444; msgs.push_back(c);
  Message x = mk('X'); x.ref = 11; x.qty = 22; msgs.push_back(x);
  Message d = mk('D'); d.ref = 11; msgs.push_back(d);
  Message u = mk('U'); u.ref = 11; u.new_ref = 12; u.qty = 13; u.price = 14; msgs.push_back(u);
  Message p = mk('P'); p.ref = 0; p.side = 'B'; p.qty = 15; p.price = 16; p.match = 17; std::snprintf(p.stock, sizeof p.stock, "AAPL"); msgs.push_back(p);
  Message q = mk('Q'); q.qty = 1ULL << 40; q.price = 18; q.match = 19; q.code = 'C'; std::snprintf(q.stock, sizeof q.stock, "AAPL"); msgs.push_back(q);

  std::vector<std::uint8_t> bytes;
  for (const auto& m : msgs) ASSERT_TRUE(append_framed(bytes, m)) << m.type;
  MemorySource src(bytes);
  FrameReader<MemorySource> fr(src, 128);
  const std::uint8_t* body;
  std::size_t len;
  std::size_t i = 0;
  while (fr.next(body, len)) {
    Message got;
    ASSERT_EQ(parse(body, len, got), ParseStatus::Ok) << "message " << i;
    EXPECT_TRUE(got == msgs[i]) << "type " << msgs[i].type;
    ++i;
  }
  EXPECT_EQ(i, msgs.size());
  EXPECT_FALSE(fr.truncated());
}

TEST(ItchFrames, TruncatedFinalFrameIsReported) {
  std::vector<std::uint8_t> bytes;
  Message d;
  d.type = 'D';
  d.ref = 1;
  append_framed(bytes, d);
  bytes.pop_back();  // chop the last byte
  MemorySource src(bytes);
  FrameReader<MemorySource> fr(src, 64);
  const std::uint8_t* body;
  std::size_t len;
  EXPECT_FALSE(fr.next(body, len));
  EXPECT_TRUE(fr.truncated());
}

// ---------------------------------------------------------------------------------- validator

namespace {

struct Feed {
  Validator v;
  std::uint64_t ts = 34'200'000'000'000ULL;
  explicit Feed(ValidateConfig cfg = {}) : v(std::move(cfg)) {}
  void send(Message m) {
    m.ts_ns = ++ts;
    if (m.locate == 0 && m.type != 'S') m.locate = 1;
    v.on_message(m);
  }
  void open() {  // directory, market hours start, opening cross
    Message r;
    r.type = 'R';
    r.locate = 1;
    std::snprintf(r.stock, sizeof r.stock, "TEST");
    send(r);
    Message s;
    s.type = 'S';
    s.code = 'Q';
    send(s);
    Message q;
    q.type = 'Q';
    q.code = 'O';
    send(q);
  }
  void add(std::uint64_t ref, char side, std::uint64_t shares, std::int64_t price) {
    Message m;
    m.type = 'A';
    m.ref = ref;
    m.side = side;
    m.qty = shares;
    m.price = price;
    std::snprintf(m.stock, sizeof m.stock, "TEST");
    send(m);
  }
  void exec(std::uint64_t ref, std::uint64_t shares) {
    Message m;
    m.type = 'E';
    m.ref = ref;
    m.qty = shares;
    send(m);
  }
  void cancel(std::uint64_t ref, std::uint64_t shares) {
    Message m;
    m.type = 'X';
    m.ref = ref;
    m.qty = shares;
    send(m);
  }
  void del(std::uint64_t ref) {
    Message m;
    m.type = 'D';
    m.ref = ref;
    send(m);
  }
  void replace(std::uint64_t ref, std::uint64_t nref, std::uint64_t shares, std::int64_t price) {
    Message m;
    m.type = 'U';
    m.ref = ref;
    m.new_ref = nref;
    m.qty = shares;
    m.price = price;
    send(m);
  }
  Stats t() const { return v.total(); }
};

std::uint64_t mismatches(const Stats& s, int bucket) {
  std::uint64_t n = 0;
  for (int k = 0; k < kMismatchKinds; ++k) n += s.mismatch[bucket][k];
  return n;
}

}  // namespace

TEST(ItchValidate, FifoExecutionsMatchTheEngine) {
  Feed f;
  f.open();
  f.add(1, 'B', 100, 100000);
  f.add(2, 'B', 50, 100000);
  f.add(3, 'S', 80, 100100);
  f.exec(1, 40);   // front of the queue
  f.exec(1, 60);   // rest of order 1
  f.exec(2, 50);   // now order 2 is first
  f.exec(3, 80);
  const Stats s = f.t();
  EXPECT_EQ(s.exec_checked[kContinuous], 4u);
  EXPECT_EQ(s.exec_ok[kContinuous], 4u);
  EXPECT_EQ(mismatches(s, kContinuous), 0u);
  for (int k = 0; k < kViolationKinds; ++k) EXPECT_EQ(s.violation[k], 0u) << k;
}

TEST(ItchValidate, ExecutionOutOfQueueOrderIsAPriorityMismatch) {
  Feed f;
  f.open();
  f.add(1, 'B', 100, 100000);
  f.add(2, 'B', 50, 100000);
  f.exec(2, 20);  // exchange executes the second order first; our engine would hit order 1
  const Stats s = f.t();
  EXPECT_EQ(s.exec_checked[kContinuous], 1u);
  EXPECT_EQ(s.exec_ok[kContinuous], 0u);
  EXPECT_EQ(s.mismatch[kContinuous][kSamePriceOtherOrder], 1u);
  EXPECT_EQ(s.rebuilds, 1u);
  ASSERT_EQ(f.v.examples().size(), 1u);
  EXPECT_EQ(f.v.examples()[0].ref, 2u);
  EXPECT_EQ(f.v.examples()[0].other_ref, 1u);
  // after the resync the book follows the exchange: order 2 has 30 left, order 1 is still first
  f.exec(1, 100);
  f.exec(2, 30);
  EXPECT_EQ(f.t().exec_ok[kContinuous], 2u);
  EXPECT_EQ(f.t().violation[kBookQtyMismatch], 0u);
}

TEST(ItchValidate, ExecutionAtAWorsePriceIsABetterPriceAheadMismatch) {
  Feed f;
  f.open();
  f.add(1, 'S', 100, 100200);
  f.add(2, 'S', 100, 100100);
  f.exec(1, 10);  // exchange executes the 10.02 offer while a 10.01 offer is resting
  const Stats s = f.t();
  EXPECT_EQ(s.mismatch[kContinuous][kBetterPriceAhead], 1u);
}

TEST(ItchValidate, ReplaceLosesPriorityPartialCancelKeepsIt) {
  Feed f;
  f.open();
  f.add(1, 'B', 100, 100000);
  f.add(2, 'B', 100, 100000);
  f.add(3, 'B', 100, 100000);
  f.replace(1, 4, 100, 100000);  // same price and size, but a replace goes to the back
  f.cancel(2, 40);               // partial cancel keeps order 2 where it was
  f.exec(2, 60);                 // 2 is now first and has 60 left
  f.exec(3, 100);
  f.exec(4, 100);
  const Stats s = f.t();
  EXPECT_EQ(s.exec_ok[kContinuous], 3u);
  EXPECT_EQ(mismatches(s, kContinuous), 0u);
  // had the engine kept priority on replace, executing 3 before 4 would still pass; executing 1's
  // replacement (4) before 3 must NOT:
  Feed g;
  g.open();
  g.add(1, 'B', 100, 100000);
  g.add(2, 'B', 100, 100000);
  g.replace(1, 3, 100, 100000);
  g.exec(3, 100);  // exchange claims the replaced order is first; replace moved it behind order 2
  EXPECT_EQ(g.t().mismatch[kContinuous][kSamePriceOtherOrder], 1u);
}

TEST(ItchValidate, OrderLevelViolationsAreCounted) {
  Feed f;
  f.open();
  f.add(1, 'B', 100, 100000);
  f.exec(99, 10);      // unknown reference
  f.del(98);           // unknown reference
  f.cancel(1, 500);    // more than remaining
  f.add(1, 'S', 5, 100500);  // duplicate reference (order 1 was cancelled to zero above and removed)
  const Stats s = f.t();
  EXPECT_EQ(s.violation[kUnknownRef], 2u);
  EXPECT_EQ(s.violation[kOverCancel], 1u);
  Feed g;
  g.open();
  g.add(1, 'B', 100, 100000);
  g.exec(1, 150);  // more than remaining
  EXPECT_EQ(g.t().violation[kOverExecute], 1u);
  Feed h;
  h.open();
  h.add(1, 'B', 100, 100000);
  h.add(1, 'B', 100, 100000);
  EXPECT_EQ(h.t().violation[kDuplicateRef], 1u);
}

TEST(ItchValidate, AddsThatWouldCrossTheBookAreCountedNotInserted) {
  Feed f;
  f.open();
  f.add(1, 'S', 100, 100000);
  f.add(2, 'B', 100, 100000);  // locks/crosses: not representable in an uncrossed book
  EXPECT_EQ(f.t().crossed_adds[kContinuous], 1u);
  f.exec(2, 50);  // executing an order that was never booked is accounted separately, not checked
  EXPECT_EQ(f.t().events_on_unbooked, 1u);
  EXPECT_EQ(f.t().exec_checked[kContinuous], 0u);
}

TEST(ItchValidate, BucketsFollowSystemEventsCrossesAndTradingState) {
  Feed f;
  Message r;
  r.type = 'R';
  r.locate = 1;
  std::snprintf(r.stock, sizeof r.stock, "TEST");
  f.send(r);
  f.add(1, 'B', 100, 100000);
  f.exec(1, 10);  // before market hours
  EXPECT_EQ(f.t().exec_checked[kPre], 1u);
  Message s;
  s.type = 'S';
  s.code = 'Q';
  f.send(s);
  f.exec(1, 10);  // market hours but the symbol has not opened
  EXPECT_EQ(f.t().exec_checked[kMarketOther], 1u);
  Message q;
  q.type = 'Q';
  q.code = 'O';
  f.send(q);
  f.exec(1, 10);
  EXPECT_EQ(f.t().exec_checked[kContinuous], 1u);
  Message h;
  h.type = 'H';
  h.code = 'H';
  std::snprintf(h.stock, sizeof h.stock, "TEST");
  f.send(h);  // halted
  f.exec(1, 10);
  EXPECT_EQ(f.t().exec_checked[kMarketOther], 2u);
  h.code = 'T';
  f.send(h);
  f.exec(1, 10);
  EXPECT_EQ(f.t().exec_checked[kContinuous], 2u);
  Message e;
  e.type = 'S';
  e.code = 'M';
  f.send(e);
  f.add(2, 'B', 10, 100000);
  f.exec(2, 5);
  EXPECT_EQ(f.t().exec_checked[kPost], 1u);
}

// ------------------------------------------------------------------------- synthetic file e2e

TEST(ItchSynthetic, FileParsesValidatesAndMatchesTheReferenceModel) {
  SynthConfig sc;
  sc.seed = 3;
  sc.ops_per_symbol = 6000;
  const auto bytes = make_synthetic_itch(sc);
  ValidateConfig cfg;
  cfg.reference_symbols = {"SYNA", "SYNB"};
  cfg.compare_every = 25;
  Validator v(cfg);
  MemorySource src(bytes);
  FrameReader<MemorySource> fr(src, 256);
  const std::uint8_t* body;
  std::size_t len;
  Message m;
  std::uint64_t n = 0;
  while (fr.next(body, len)) {
    ASSERT_EQ(parse(body, len, m), ParseStatus::Ok);
    v.on_message(m);
    ++n;
  }
  v.finish();
  EXPECT_FALSE(fr.truncated());
  EXPECT_GT(n, 8000u);
  const Stats s = v.total();
  EXPECT_EQ(s.messages + 6u, n);  // 6 system-event messages carry no stock locate
  EXPECT_GT(s.exec_checked[kContinuous], 500u);
  EXPECT_EQ(s.exec_ok[kContinuous], s.exec_checked[kContinuous]);
  EXPECT_EQ(mismatches(s, kContinuous), 0u);
  EXPECT_GT(s.partial_cancels, 0u);
  EXPECT_GT(s.replaces, 0u);
  EXPECT_GT(s.deletes, 0u);
  for (int k = 0; k < kViolationKinds; ++k) EXPECT_EQ(s.violation[k], 0u) << k;
  EXPECT_EQ(s.crossed_adds[kContinuous], 0u);
  EXPECT_EQ(s.rebuilds, 0u);
  EXPECT_GT(s.ref_compares, 100u);
  EXPECT_EQ(s.ref_level_mismatch + s.ref_queue_mismatch, 0u);
  EXPECT_GT(v.system_event_ts('Q'), 0u);
  EXPECT_NE(v.to_json("synthetic").find("\"engine_matched_exchange_choice\""), std::string::npos);
}

TEST(ItchSynthetic, GenerationIsDeterministic) {
  SynthConfig sc;
  sc.ops_per_symbol = 500;
  EXPECT_EQ(make_synthetic_itch(sc), make_synthetic_itch(sc));
  SynthConfig other = sc;
  other.seed = 9;
  EXPECT_NE(make_synthetic_itch(sc), make_synthetic_itch(other));
}
