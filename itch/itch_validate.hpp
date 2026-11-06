#pragma once

// Replays an ITCH 5.0 message stream through lob::OrderBook and validates the engine against it.
// Mapping and checks are documented in docs/REALDATA.md.
//
//  (a) order-level consistency: every E/C/X/D/U must reference a live order, executions and
//      cancels cannot exceed the remaining size, and the remaining size inside OUR book must equal
//      what the exchange implies after each message;
//  (b) priority check: for every E/C execution a marketable IOC for the executed size is submitted to
//      OUR book; the engine, not the data, decides which resting order it hits, and that must be the
//      order the exchange executed (same id, price, size). Disagreements are classified;
//  (c) independent cross-check: for selected symbols the same exchange-truth stream also feeds the
//      slow reference book (std::map + std::deque, Stage 2), and BBO, every level aggregate and every
//      level's queue order are compared against the optimised book every N messages.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lob/itch.hpp"
#include "lob/order_book.hpp"
#include "reference_book.hpp"

namespace lob::itch {

enum Bucket : int { kPre = 0, kContinuous = 1, kMarketOther = 2, kPost = 3, kBuckets = 4 };
enum MismatchKind : int { kNoFill = 0, kBetterPriceAhead = 1, kSamePriceOtherOrder = 2, kOtherDifference = 3, kMismatchKinds = 4 };
enum ViolationKind : int {
  kUnknownRef = 0,       ///< E/C/X/D/U for a reference that is not live
  kDuplicateRef = 1,     ///< A/F/U reusing a live reference
  kOverExecute = 2,      ///< executed more than the remaining size
  kOverCancel = 3,       ///< cancelled more than the remaining size
  kBookQtyMismatch = 4,  ///< our book's remaining size differs from the exchange-implied size
  kBookRejected = 5,     ///< our book rejected an operation the exchange performed
  kViolationKinds = 6
};

struct Stats {
  std::uint64_t messages = 0;
  std::uint64_t adds = 0, deletes = 0, partial_cancels = 0, replaces = 0, exec_e = 0, exec_c = 0;
  std::uint64_t c_not_printable = 0, c_price_differs = 0, trades_p = 0, crosses_q = 0, trading_actions = 0;
  std::uint64_t exec_shares = 0;
  std::uint64_t exec_checked[kBuckets] = {}, exec_ok[kBuckets] = {};
  std::uint64_t mismatch[kBuckets][kMismatchKinds] = {};
  std::uint64_t mismatch_odd_lot[kBuckets] = {};  ///< mismatches where the executed or the preferred order is an odd lot
  std::uint64_t crossed_adds[kBuckets] = {};      ///< adds that would cross or lock our uncrossed book (not inserted)
  std::uint64_t events_on_unbooked = 0;
  std::uint64_t violation[kViolationKinds] = {};
  std::uint64_t rebuilds = 0;
  std::uint64_t ref_compares = 0, ref_level_mismatch = 0, ref_queue_mismatch = 0;
  Stats& operator+=(const Stats& o);
};

struct ValidateConfig {
  std::set<std::string> reference_symbols;  ///< also run the reference book and compare (check c)
  std::uint64_t compare_every = 2000;       ///< per-symbol messages between reference comparisons
  std::uint64_t max_rebuilds_per_symbol = 20000;
  std::size_t max_examples = 25;
};

struct Example {
  std::string symbol;
  std::uint64_t ts_ns = 0, ref = 0, other_ref = 0;
  std::int64_t price = 0, other_price = 0;
  std::uint64_t executed = 0, order_shares_orig = 0, other_shares_orig = 0;
  int bucket = 0, kind = 0;
  char side = 0;
};

class Validator {
 public:
  explicit Validator(ValidateConfig cfg = {});
  ~Validator();
  void on_message(const Message& m);
  /// Final reference comparisons for the selected symbols.
  void finish();

  Stats total() const;
  struct SymbolInfo {
    std::string name;
    Stats stats;
    bool reference = false, opened = false, closed = false, diverged = false;
    std::uint64_t resting_orders = 0;
  };
  std::vector<SymbolInfo> symbols() const;
  const std::vector<Example>& examples() const { return examples_; }
  std::uint64_t system_event_ts(char code) const;
  std::array<std::uint64_t, 256> counts_by_type() const { return by_type_; }
  std::string to_json(const std::string& file_label) const;

 private:
  struct Order {
    std::uint32_t locate = 0;
    char side = 'B';
    std::int64_t price = 0;
    std::uint32_t remaining = 0, original = 0;
    std::uint64_t seq = 0;
    bool booked = true;
  };
  struct Symbol;
  Symbol& sym(std::uint16_t locate);
  int bucket_of(const Symbol& s) const;
  void add_order(Symbol& s, std::uint64_t ref, char side, std::int64_t price, std::uint32_t shares);
  void remove_order(Symbol& s, std::uint64_t ref, Order& o);
  void execute(Symbol& s, const Message& m);
  void rebuild(Symbol& s);
  void compare_reference(Symbol& s);

  ValidateConfig cfg_;
  std::unordered_map<std::uint16_t, std::unique_ptr<Symbol>> syms_;
  std::unordered_map<std::uint64_t, Order> orders_;
  std::array<std::uint64_t, 256> by_type_{};
  std::map<char, std::uint64_t> system_events_;
  int phase_ = 0;  // 0 pre-market, 1 market hours, 2 post
  std::uint64_t seq_ = 0, ioc_id_ = 1ULL << 62, first_ts_ = 0, last_ts_ = 0;
  std::vector<Example> examples_;
};

}  // namespace lob::itch
