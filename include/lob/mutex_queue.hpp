#pragma once

// Bounded blocking queue (std::mutex + two condition variables). Baseline for the SPSC ring:
// same capacity semantics, same element type, but every operation takes a lock and a blocked side
// sleeps in the kernel. Multi-producer / multi-consumer safe (we use it with one of each).
//
// To keep the baseline fair (not artificially slow) it only calls notify when the other side may
// actually be waiting: the producer notifies on the empty->non-empty transition, the consumer on
// the full->not-full transition.

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace lob {

template <class T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : buf_(capacity ? capacity : 1) {}

  /// Blocks while the queue is full.
  void push(const T& v) {
    std::unique_lock<std::mutex> lk(m_);
    not_full_.wait(lk, [&] { return count_ < buf_.size(); });
    const bool was_empty = count_ == 0;
    buf_[(head_ + count_) % buf_.size()] = v;
    ++count_;
    lk.unlock();
    if (was_empty) not_empty_.notify_one();
  }

  /// Blocks while the queue is empty.
  void pop(T& out) {
    std::unique_lock<std::mutex> lk(m_);
    not_empty_.wait(lk, [&] { return count_ > 0; });
    const bool was_full = count_ == buf_.size();
    out = buf_[head_];
    head_ = (head_ + 1) % buf_.size();
    --count_;
    lk.unlock();
    if (was_full) not_full_.notify_one();
  }

 private:
  std::mutex m_;
  std::condition_variable not_empty_, not_full_;
  std::vector<T> buf_;
  std::size_t head_ = 0, count_ = 0;
};

}  // namespace lob
