#pragma once

// FNV-1a digest of an event stream and of the final book state. Identical to the digest the
// Stage 3 benchmark stores in its result files, so a checksum from any harness can be compared
// with any other.

#include <cstdint>

#include "lob/order_book.hpp"

namespace lob {

inline std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= 1099511628211ULL;
  }
  return h;
}

struct EventHasher {
  std::uint64_t h = 14695981039346656037ULL;
  void operator()(const Event& e) {
    h = fnv_mix(h, static_cast<std::uint64_t>(e.type));
    h = fnv_mix(h, e.order_id);
    h = fnv_mix(h, e.maker_id);
    h = fnv_mix(h, static_cast<std::uint64_t>(e.side));
    h = fnv_mix(h, static_cast<std::uint64_t>(e.price));
    h = fnv_mix(h, static_cast<std::uint64_t>(e.qty));
    h = fnv_mix(h, static_cast<std::uint64_t>(e.reject));
    h = fnv_mix(h, static_cast<std::uint64_t>(e.cancel));
    h = fnv_mix(h, e.lost_priority ? 1 : 0);
  }
};

/// Folds the whole resting book (levels best-first, orders in time priority) into `h`.
inline std::uint64_t digest_book(std::uint64_t h, const OrderBook& book) {
  for (Side s : {Side::Buy, Side::Sell}) {
    for (const auto& lvl : book.levels(s)) {
      h = fnv_mix(h, static_cast<std::uint64_t>(lvl.price));
      for (const auto& o : book.orders_at(s, lvl.price)) {
        h = fnv_mix(h, o.id);
        h = fnv_mix(h, static_cast<std::uint64_t>(o.qty));
      }
    }
  }
  return h;
}

}  // namespace lob
