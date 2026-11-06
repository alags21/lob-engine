// TSan negative control: a producer/consumer pair over SpscQueue built with the hand-off orderings
// downgraded to relaxed (LOB_SPSC_RACY_FOR_TSAN_DEMO). Under ThreadSanitizer this program must be
// reported as a data race on the queue slots; CI asserts exactly that. Not part of the default
// build or of ctest (enable with -DLOB_RACY_DEMO=ON).
#include <cstdint>
#include <cstdio>
#include <thread>

#include "lob/spsc_queue.hpp"

int main() {
  lob::SpscQueue<std::uint64_t> q(8);
  constexpr std::uint64_t n = 200000;
  std::uint64_t sum = 0;
  std::thread consumer([&] {
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      while (!q.try_pop(v)) std::this_thread::yield();
      sum += v;
    }
  });
  for (std::uint64_t i = 0; i < n; ++i)
    while (!q.try_push(i)) std::this_thread::yield();
  consumer.join();
  std::printf("sum=%llu\n", static_cast<unsigned long long>(sum));
  return 0;
}
