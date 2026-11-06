#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "lob/checksum.hpp"
#include "lob/event_log.hpp"
#include "lob/mutex_queue.hpp"
#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"

using namespace lob;

namespace {

// Payload whose fields are all derived from `seq`, so a torn or stale read is detectable.
struct Payload {
  std::uint64_t seq;
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
};
Payload make_payload(std::uint64_t i) { return {i, i * 3 + 1, ~i, i ^ 0xABCDEF12345ULL}; }
bool valid(const Payload& p, std::uint64_t expect_seq) {
  const Payload e = make_payload(expect_seq);
  return p.seq == e.seq && p.a == e.a && p.b == e.b && p.c == e.c;
}

void spin_for(unsigned n) {
  for (volatile unsigned i = 0; i < n; i = i + 1) {
  }
}

enum class Skew { None, SlowConsumer, SlowProducer };

// Pushes `n` payloads through a queue of the given capacity and checks order and integrity.
template <class Q>
void stress(std::size_t capacity, std::uint64_t n, Skew skew) {
  Q q(capacity);
  std::atomic<bool> ok{true};
  std::uint64_t consumed = 0;
  std::thread consumer([&] {
    Payload p{};
    for (std::uint64_t i = 0; i < n; ++i) {
      while (!q.try_pop(p)) std::this_thread::yield();
      if (!valid(p, i)) ok.store(false, std::memory_order_relaxed);
      if (skew == Skew::SlowConsumer && (i & 0xFF) == 0) spin_for(2000);
      ++consumed;
    }
  });
  for (std::uint64_t i = 0; i < n; ++i) {
    const Payload p = make_payload(i);
    while (!q.try_push(p)) std::this_thread::yield();
    if (skew == Skew::SlowProducer && (i & 0xFF) == 0) spin_for(2000);
  }
  consumer.join();
  EXPECT_TRUE(ok.load());
  EXPECT_EQ(consumed, n);
  Payload leftover{};
  EXPECT_FALSE(q.try_pop(leftover));  // fully drained
}

}  // namespace

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ((SpscQueue<int>(0).capacity()), 2u);
  EXPECT_EQ((SpscQueue<int>(1).capacity()), 2u);
  EXPECT_EQ((SpscQueue<int>(2).capacity()), 2u);
  EXPECT_EQ((SpscQueue<int>(3).capacity()), 4u);
  EXPECT_EQ((SpscQueue<int>(1000).capacity()), 1024u);
}

TEST(SpscQueue, EmptyPopFailsAndFullPushFails) {
  SpscQueue<int> q(4);
  int v = 0;
  EXPECT_FALSE(q.try_pop(v));
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));  // full
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, WrapsAroundManyTimesSingleThreaded) {
  SpscQueue<std::uint64_t> q(4);
  std::uint64_t next_in = 0, next_out = 0, v = 0;
  for (int round = 0; round < 10000; ++round) {
    const int burst = 1 + round % 4;  // 1..4, exercises every fill level
    for (int i = 0; i < burst; ++i) ASSERT_TRUE(q.try_push(next_in++));
    for (int i = 0; i < burst; ++i) {
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, next_out++);
    }
  }
}

TEST(SpscQueue, UnpaddedVariantBehavesIdentically) {
  SpscQueue<int, false> q(2);
  int v = 0;
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(q.try_push(3));
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 2);
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 3);
}

// Tiny capacities force constant full/empty transitions and index wraparound; the skews make
// either side the bottleneck. Item counts are kept moderate so the test also finishes under
// ThreadSanitizer, which slows execution by roughly an order of magnitude.
TEST(SpscStress, TinyCapacitiesBothSkews) {
  for (std::size_t cap : {2u, 4u, 8u}) {
    for (Skew s : {Skew::None, Skew::SlowConsumer, Skew::SlowProducer}) {
      stress<SpscQueue<Payload, true>>(cap, 200000, s);
      stress<SpscQueue<Payload, false>>(cap, 200000, s);
    }
  }
}

TEST(SpscStress, ManyShortRuns) {
  for (int iter = 0; iter < 300; ++iter) stress<SpscQueue<Payload, true>>(2, 2000, Skew::None);
}

TEST(MutexQueueStress, TinyCapacities) {
  for (std::size_t cap : {1u, 2u, 8u}) {
    MutexQueue<Payload> q(cap);
    const std::uint64_t n = 50000;
    std::atomic<bool> ok{true};
    std::thread consumer([&] {
      Payload p{};
      for (std::uint64_t i = 0; i < n; ++i) {
        q.pop(p);
        if (!valid(p, i)) ok.store(false);
      }
    });
    for (std::uint64_t i = 0; i < n; ++i) q.push(make_payload(i));
    consumer.join();
    EXPECT_TRUE(ok.load());
  }
}

// ---- Pipeline: producer -> SPSC queue -> matching thread must equal the single-thread run ----

namespace {

std::vector<Op> make_ops(std::uint64_t seed, std::size_t n) {
  // Dense crossing flow around a small price band; exercises every op kind and tif.
  std::vector<Op> ops;
  std::uint64_t s = seed;
  auto rnd = [&](std::uint64_t m) {
    s += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (z ^ (z >> 31)) % m;
  };
  OrderId next = 1;
  for (std::size_t i = 0; i < n; ++i) {
    Op op;
    const auto r = rnd(100);
    if (r < 60) {
      op.kind = Op::Kind::Submit;
      op.id = next++;
      op.side = rnd(2) ? Side::Buy : Side::Sell;
      op.price = 100 + static_cast<Price>(rnd(7)) - 3;
      op.qty = 1 + static_cast<Qty>(rnd(15));
      op.type = rnd(10) == 0 ? OrderType::Market : OrderType::Limit;
      const auto t = rnd(10);
      op.tif = t < 7 ? TimeInForce::GTC : t < 9 ? TimeInForce::IOC : TimeInForce::FOK;
    } else if (r < 85) {
      op.kind = Op::Kind::Cancel;
      op.id = 1 + rnd(next);
    } else {
      op.kind = Op::Kind::Modify;
      op.id = 1 + rnd(next);
      op.price = 100 + static_cast<Price>(rnd(7)) - 3;
      op.qty = 1 + static_cast<Qty>(rnd(15));
    }
    ops.push_back(op);
  }
  return ops;
}

std::uint64_t run_single_thread(const std::vector<Op>& ops) {
  EventHasher hasher;
  OrderBook book([&](const Event& e) { hasher(e); });
  for (const Op& op : ops) apply(book, op);
  return digest_book(hasher.h, book);
}

template <class Q, class Push, class Pop>
std::uint64_t run_pipeline(const std::vector<Op>& ops, Q& q, Push push, Pop pop) {
  EventHasher hasher;
  OrderBook book([&](const Event& e) { hasher(e); });
  std::thread consumer([&] {
    Op op;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      pop(q, op);
      apply(book, op);
    }
  });
  for (const Op& op : ops) push(q, op);
  consumer.join();
  return digest_book(hasher.h, book);
}

}  // namespace

TEST(Pipeline, SpscMatchesSingleThreadForManySeedsAndCapacities) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    const auto ops = make_ops(seed, 60000);
    const std::uint64_t expected = run_single_thread(ops);
    for (std::size_t cap : {2u, 16u, 1024u}) {
      SpscQueue<Op> q(cap);
      const auto got = run_pipeline(
          ops, q, [](SpscQueue<Op>& qq, const Op& o) { while (!qq.try_push(o)) std::this_thread::yield(); },
          [](SpscQueue<Op>& qq, Op& o) { while (!qq.try_pop(o)) std::this_thread::yield(); });
      EXPECT_EQ(got, expected) << "seed " << seed << " capacity " << cap;
    }
  }
}

TEST(Pipeline, MutexQueueMatchesSingleThread) {
  const auto ops = make_ops(42, 60000);
  const std::uint64_t expected = run_single_thread(ops);
  MutexQueue<Op> q(16);
  const auto got = run_pipeline(
      ops, q, [](MutexQueue<Op>& qq, const Op& o) { qq.push(o); },
      [](MutexQueue<Op>& qq, Op& o) { qq.pop(o); });
  EXPECT_EQ(got, expected);
}
