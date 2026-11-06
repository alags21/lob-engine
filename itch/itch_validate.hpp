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
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lob/event_log.hpp"
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
  // Taxonomy of continuous-hours priority mismatches (same-price case). R = order the exchange executed,
  // W = order our FIFO would have executed instead.
  std::uint64_t tax_ahead_all_odd = 0, tax_ahead_has_round = 0;  // orders ahead of R at that price (by arrival): all odd lots / at least one round lot
  std::uint64_t tax_r_attributed = 0, tax_w_attributed = 0, tax_r_from_replace = 0, tax_w_from_replace = 0;
  std::uint64_t tax_r_odd = 0, tax_w_odd = 0, tax_both_round = 0;
  std::uint64_t tax_gap_le_1us = 0, tax_gap_le_1ms = 0, tax_gap_le_1s = 0, tax_gap_gt_1s = 0;  // time between the two orders' adds
  std::uint64_t tax_w_exec_le_1us = 0, tax_w_exec_le_1ms = 0, tax_w_exec_later = 0, tax_w_changed = 0, tax_w_pending_end = 0;
  std::uint64_t ref_compares = 0, ref_level_mismatch = 0, ref_queue_mismatch = 0;
  Stats& operator+=(const Stats& o);
};

struct ValidateConfig {
  std::set<std::string> reference_symbols;  ///< also run the reference book and compare (check c)
  std::uint64_t compare_every = 2000;       ///< per-symbol messages between reference comparisons
  std::uint64_t max_rebuilds_per_symbol = 20000;
  std::size_t max_examples = 25;
  /// Experiment: treat an order replace (U) at the same price with a size that does not increase as keeping
  /// queue priority (the engine's own modify rule) instead of always going to the back of the queue.
  bool replace_reduction_keeps_priority = false;
  /// Experiment: treat a partial cancel (X) as losing queue priority (cancel + re-add at the back).
  bool partial_cancel_loses_priority = false;
  /// If non-empty, only these symbols are processed (other order messages are skipped).
  std::set<std::string> only_symbols;
  /// Called for every operation applied to the engine under test (adds, cancels, modifies, the IOC
  /// used for executions, and the re-submissions of a rebuild), so the exact op stream the
  /// validation saw can be replayed later for benchmarking.
  std::function<void(std::uint16_t locate, const lob::Op&)> op_sink;
  /// Called when a symbol's book is discarded and rebuilt after a disagreement.
  std::function<void(std::uint16_t locate)> reset_sink;
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
  /// FNV digest of the symbol's final resting book (levels best first, orders in priority order).
  std::uint64_t book_digest(const std::string& symbol) const;

 private:
  struct Order {
    std::uint32_t locate = 0;
    char side = 'B';
    std::int64_t price = 0;
    std::uint32_t remaining = 0, original = 0;
    std::uint64_t seq = 0;
    std::uint64_t add_ts = 0;
    bool attributed = false, from_replace = false;
    std::uint64_t eng_id = 0;  ///< id of this order inside our engine (differs from the ITCH reference after an in-place replace)
    bool booked = true;
  };
  struct Symbol;
  Symbol& sym(std::uint16_t locate);
  int bucket_of(const Symbol& s) const;
  void add_order(Symbol& s, std::uint64_t ref, char side, std::int64_t price, std::uint32_t shares, bool attributed = false, bool from_replace = false);
  void remove_order(Symbol& s, std::uint64_t ref, Order& o);
  void execute(Symbol& s, const Message& m);
  void rebuild(Symbol& s);
  void compare_reference(Symbol& s);
  void emit_op(const Symbol& s, const lob::Op& op);

  ValidateConfig cfg_;
  std::unordered_map<std::uint16_t, std::unique_ptr<Symbol>> syms_;
  std::unordered_map<std::uint64_t, Order> orders_;
  std::array<std::uint64_t, 256> by_type_{};
  std::map<char, std::uint64_t> system_events_;
  int phase_ = 0;  // 0 pre-market, 1 market hours, 2 post
  std::uint64_t seq_ = 0, ioc_id_ = 1ULL << 62, first_ts_ = 0, last_ts_ = 0;
  std::vector<Example> examples_;
  std::unordered_map<std::uint64_t, std::uint64_t> renamed_;  // engine id -> current ITCH reference (in-place replaces only)
};

}  // namespace lob::itch
