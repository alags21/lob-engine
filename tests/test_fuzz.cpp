#include <gtest/gtest.h>

#include "fuzz.hpp"

using namespace lob;

TEST(Fuzz, GeneratorIsDeterministicPerSeed) {
  fuzz::OpGenerator a(123), b(123), c(124);
  bool differs = false;
  for (int i = 0; i < 1000; ++i) {
    const Op x = a.next(), y = b.next(), z = c.next();
    EXPECT_EQ(x, y);
    differs |= !(x == z);
  }
  EXPECT_TRUE(differs);
}

// Short differential run so every ctest invocation exercises the oracle. The long runs are done
// by the lob_fuzz executable in CI.
TEST(Fuzz, DifferentialAgainstReferenceShort) {
  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    const auto r = fuzz::run_fuzz({seed, 40000});
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_EQ(r.ops, 40000u);
    EXPECT_GT(r.trades, 0u);  // the stream must actually cross, otherwise the test proves nothing
  }
}
