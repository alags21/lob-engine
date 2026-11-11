#pragma once

// Bounded single-producer / single-consumer lock-free ring buffer.
//
// Exactly one thread may call try_push and exactly one (other) thread may call try_pop.
//
// Indices are free-running std::size_t counters (never reduced modulo the capacity); a slot is
// addressed with `index & mask` (capacity is a power of two). The number of queued items is
// tail - head, so the queue is full when tail - head == capacity and empty when tail == head.
// 64-bit counters do not wrap in practice.
//
// Memory-ordering argument (the only synchronisation is two release/acquire pairs):
//
//  * tail_  is written only by the producer, with `release`, AFTER it has written the slot
//    contents. The consumer reads tail_ with `acquire`. If the consumer observes tail_ == t+1, the
//    acquire load synchronises with that release store, so the write of slot t happens-before the
//    consumer's read of slot t: it sees a fully written item (no torn read).
//  * head_  is written only by the consumer, with `release`, AFTER it has finished reading the
//    slot. The producer reads head_ with `acquire`. If the producer observes head_ == h+1, the
//    consumer's read of slot h happens-before the producer's later overwrite of that slot (which
//    occurs one lap later, at index h + capacity): no overwrite of data still being read.
//  * Each side reads its OWN index with `relaxed`: it is the only writer, so it always sees its
//    own latest value, and no ordering against the other thread is needed for that load.
//  * cached_head_ (producer-private) and cached_tail_ (consumer-private) are plain, non-atomic
//    copies of the other side's index. They are a conservative lower bound on the progress of the
//    other side (head and tail only increase), so "full according to the cache" is re-checked with
//    a real acquire load before giving up, and "has room/data according to the cache" is always
//    true. They exist to avoid touching the other core's cache line on every operation: the shared
//    index line is only fetched when the cached value says the queue is full (producer) or empty
//    (consumer).
//
// No operation needs seq_cst: there is no multi-variable property that requires a total order,
// only the two producer->consumer / consumer->producer hand-offs above.
//
// Padded = true puts the producer-owned fields (tail_, cached_head_) and the consumer-owned fields
// (head_, cached_tail_) on separate cache lines, so a store by one side does not invalidate the
// line the other side is spinning on (false sharing). Padded = false packs all four together; it
// exists only as an ablation for the benchmark.

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // structure was padded due to alignment specifier (intended)
#endif

namespace lob {

/// Destructive-interference size used for padding. 64 bytes on x86-64; Apple M-series use 128.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

namespace detail {

// TEST-ONLY: building with LOB_SPSC_RACY_FOR_TSAN_DEMO downgrades the hand-off orderings to
// relaxed, which makes the queue racy on purpose. It exists solely so CI can prove that
// ThreadSanitizer reports the race (see tests/racy_demo.cpp). Never define it in a real build.
#ifdef LOB_SPSC_RACY_FOR_TSAN_DEMO
inline constexpr std::memory_order kHandoffLoad = std::memory_order_relaxed;
inline constexpr std::memory_order kHandoffStore = std::memory_order_relaxed;
#else
inline constexpr std::memory_order kHandoffLoad = std::memory_order_acquire;
inline constexpr std::memory_order kHandoffStore = std::memory_order_release;
#endif

struct PaddedIndices {
  alignas(kCacheLine) std::atomic<std::size_t> tail{0};  // written by producer
  std::size_t cached_head = 0;                            // producer-private
  alignas(kCacheLine) std::atomic<std::size_t> head{0};  // written by consumer
  std::size_t cached_tail = 0;                            // consumer-private
};

struct PackedIndices {
  std::atomic<std::size_t> tail{0};
  std::size_t cached_head = 0;
  std::atomic<std::size_t> head{0};
  std::size_t cached_tail = 0;
};

constexpr std::size_t round_up_pow2(std::size_t n) {
  std::size_t p = 2;
  while (p < n) p <<= 1;
  return p;
}

}  // namespace detail

template <class T, bool Padded = true>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "slots are copied without synchronisation beyond the indices");

 public:
  /// Capacity is rounded up to a power of two, minimum 2.
  explicit SpscQueue(std::size_t min_capacity)
      : capacity_(detail::round_up_pow2(min_capacity)),
        mask_(capacity_ - 1),
        buf_(new T[capacity_]()) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  std::size_t capacity() const noexcept { return capacity_; }

  /// Producer thread only. Returns false if the queue is full.
  bool try_push(const T& v) noexcept {
    const std::size_t t = ix_.tail.load(std::memory_order_relaxed);
    if (t - ix_.cached_head == capacity_) {
      ix_.cached_head = ix_.head.load(detail::kHandoffLoad);
      if (t - ix_.cached_head == capacity_) return false;
    }
    buf_[t & mask_] = v;
    ix_.tail.store(t + 1, detail::kHandoffStore);
    return true;
  }

  /// Consumer thread only. Returns false if the queue is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t h = ix_.head.load(std::memory_order_relaxed);
    if (h == ix_.cached_tail) {
      ix_.cached_tail = ix_.tail.load(detail::kHandoffLoad);
      if (h == ix_.cached_tail) return false;
    }
    out = buf_[h & mask_];
    ix_.head.store(h + 1, detail::kHandoffStore);
    return true;
  }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> buf_;
  std::conditional_t<Padded, detail::PaddedIndices, detail::PackedIndices> ix_;
};

}  // namespace lob

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
