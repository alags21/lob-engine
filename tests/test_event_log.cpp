#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fuzz.hpp"
#include "lob/event_log.hpp"
#include "lob/order_book.hpp"
#include "reference_book.hpp"

using namespace lob;

namespace {

std::vector<std::string> read_lines(const std::string& path) {
  std::ifstream f(path);
  EXPECT_TRUE(f) << "cannot open " << path;
  std::vector<std::string> out;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

template <class Book>
std::vector<std::string> replay(const std::vector<std::string>& log) {
  std::vector<std::string> out;
  Book book([&](const Event& e) { out.push_back(to_string(e)); });
  for (const auto& line : log) {
    if (auto op = parse_op(line)) apply(book, *op);
  }
  return out;
}

}  // namespace

TEST(EventLog, OpRoundTrip) {
  fuzz::OpGenerator gen(7);
  for (int i = 0; i < 5000; ++i) {
    const Op op = gen.next();
    const auto parsed = parse_op(to_string(op));
    ASSERT_TRUE(parsed) << to_string(op);
    // Fields not used by a kind are not serialised, so compare via the text form.
    EXPECT_EQ(to_string(*parsed), to_string(op));
  }
}

TEST(EventLog, SubmitOpFieldsParse) {
  const auto op = parse_op("S 42 S M FOK 0 17");
  ASSERT_TRUE(op);
  EXPECT_EQ(op->kind, Op::Kind::Submit);
  EXPECT_EQ(op->id, 42u);
  EXPECT_EQ(op->side, Side::Sell);
  EXPECT_EQ(op->type, OrderType::Market);
  EXPECT_EQ(op->tif, TimeInForce::FOK);
  EXPECT_EQ(op->qty, 17);
}

TEST(EventLog, ParseRejectsMalformedAndSkipsComments) {
  EXPECT_FALSE(parse_op(""));
  EXPECT_FALSE(parse_op("# comment"));
  EXPECT_FALSE(parse_op("S 1 B L GTC 100"));      // missing qty
  EXPECT_FALSE(parse_op("S 1 X L GTC 100 5"));    // bad side
  EXPECT_FALSE(parse_op("S 1 B L DAY 100 5"));    // bad tif
  EXPECT_FALSE(parse_op("C abc"));
  EXPECT_FALSE(parse_op("M 1 100"));
  EXPECT_FALSE(parse_op("Z 1"));
  EXPECT_FALSE(parse_event("T 1 2 B 100"));
  EXPECT_FALSE(parse_event("D 1 B 100 5 2"));
}

TEST(EventLog, EventRoundTripThroughFuzzStream) {
  std::vector<Event> events;
  OrderBook book([&](const Event& e) { events.push_back(e); });
  fuzz::OpGenerator gen(11);
  for (int i = 0; i < 5000; ++i) apply(book, gen.next());
  ASSERT_FALSE(events.empty());
  for (const auto& e : events) {
    const auto parsed = parse_event(to_string(e));
    ASSERT_TRUE(parsed) << to_string(e);
    EXPECT_EQ(*parsed, e) << to_string(e);
  }
}

TEST(EventLog, GoldenReplayMatchesCommittedEvents) {
  const std::string dir = LOB_TEST_DATA_DIR;
  const auto log = read_lines(dir + "/golden.log");
  const auto expected = read_lines(dir + "/golden.events");
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(replay<OrderBook>(log), expected);
  EXPECT_EQ(replay<ref::ReferenceBook>(log), expected);
}

TEST(EventLog, ReplayIsDeterministic) {
  std::vector<Op> ops;
  fuzz::OpGenerator gen(99);
  std::vector<std::string> log;
  for (int i = 0; i < 20000; ++i) log.push_back(to_string(gen.next()));
  EXPECT_EQ(replay<OrderBook>(log), replay<OrderBook>(log));
}

TEST(EventLog, WrittenLogReplaysToSameEventsAsDirectRun) {
  const std::string path = std::string(::testing::TempDir()) + "lob_op_log_test.log";
  std::vector<Op> ops;
  fuzz::OpGenerator gen(5);
  for (int i = 0; i < 3000; ++i) ops.push_back(gen.next());
  ASSERT_TRUE(fuzz::write_op_log(path, ops, 5));

  std::vector<std::string> direct;
  OrderBook book([&](const Event& e) { direct.push_back(to_string(e)); });
  for (const auto& op : ops) apply(book, op);

  EXPECT_EQ(replay<OrderBook>(read_lines(path)), direct);
}
