#pragma once

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob {

/// Single-threaded limit order book with price-time priority matching.
///
/// Semantics (see docs/DESIGN.md for the rationale):
///  - Trades execute at the resting (maker) order's price.
///  - Orders at the same price fill in arrival order (FIFO).
///  - There is no self-trade prevention: the book has no notion of participant, so an order
///    that crosses an earlier order with a different id trades with it.
///  - Every call returns RejectReason::None on success; failures also emit a Rejected event.
///  - Events are delivered synchronously, in occurrence order, through the handler.
class OrderBook {
 public:
  using EventHandler = std::function<void(const Event&)>;

  explicit OrderBook(EventHandler handler = {});
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  /// Enter a new order. Emits Accepted, then any Trades, then Cancelled for an unfilled
  /// IOC/market remainder or an unfillable FOK.
  RejectReason submit(const NewOrder& order);

  /// Remove a live order. Emits Cancelled(UserRequest) with the remaining quantity.
  /// Cancelling a filled, cancelled or unknown id is rejected with UnknownOrder.
  RejectReason cancel(OrderId id);

  /// Change a live order's price and remaining quantity (`new_qty` is the new *remaining*
  /// quantity, not a delta). A reduction in size at the same price keeps queue priority and is
  /// done in place. Any price change or size increase is cancel/replace: the order goes to the
  /// back of the queue at its new price and, if it now crosses, trades as a taker. Same price
  /// and same size is a no-op that keeps priority. new_qty and new_price must be > 0.
  /// Emits Modified, followed by any Trades.
  RejectReason modify(OrderId id, Price new_price, Qty new_qty);

  // ---- Inspection (does not change the book) ----
  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  bool contains(OrderId id) const { return index_.contains(id); }

  struct OrderView {
    Side side;
    Price price;
    Qty qty;
  };
  /// Resting state of a live order.
  std::optional<OrderView> find(OrderId id) const;
  /// Total resting quantity at a price (0 if no level).
  Qty qty_at(Side side, Price price) const;
  /// Resting orders at a level in time priority (front fills first).
  std::vector<RestingOrder> orders_at(Side side, Price price) const;
  /// Levels from best to worst.
  std::vector<LevelInfo> levels(Side side) const;
  /// Checks the book's internal invariants: no crossed book, no empty level or zero/negative
  /// quantity, level totals match their orders, and the id index matches the levels exactly.
  /// Returns an empty string if all hold, otherwise a description of the first violation.
  /// (FIFO order is not checkable from state alone; it is verified by differential fuzzing.)
  /// In debug builds (NDEBUG undefined) every public mutating call runs this and aborts on failure.
  std::string validate() const;
  std::size_t order_count() const { return index_.size(); }
  bool empty() const { return index_.empty(); }

 public:
  /// Used by the debug-build guard; aborts if validate() fails.
  void debug_check_public() const;

 private:
  // Orders are intrusive nodes in a per-level doubly linked list (front = highest time priority).
  struct Node {
    OrderId id;
    Qty qty;
    Price price;
    Side side;
    Node* prev;
    Node* next;
  };
  struct Level {
    Node* head = nullptr;
    Node* tail = nullptr;
    Qty total = 0;
    std::size_t count = 0;
  };
  // Open-addressing id -> Node* table (linear probing, power-of-two capacity, load <= 1/2,
  // backward-shift deletion so there are no tombstones). A null value marks an empty slot.
  class IdMap {
   public:
    IdMap() : slots_(kInitial), mask_(kInitial - 1) {}
    Node* find(OrderId id) const {
      for (std::size_t i = hash(id) & mask_;; i = (i + 1) & mask_) {
        const Slot& s = slots_[i];
        if (!s.node) return nullptr;
        if (s.id == id) return s.node;
      }
    }
    bool contains(OrderId id) const { return find(id) != nullptr; }
    void insert(OrderId id, Node* n) {  // id must not be present
      if ((size_ + 1) * 2 > slots_.size()) grow();
      place(id, n);
      ++size_;
    }
    void erase(OrderId id) {  // id must be present
      std::size_t i = hash(id) & mask_;
      while (slots_[i].id != id || !slots_[i].node) i = (i + 1) & mask_;
      std::size_t j = i;
      for (;;) {
        j = (j + 1) & mask_;
        if (!slots_[j].node) break;
        const std::size_t home = hash(slots_[j].id) & mask_;
        // Move slot j into the hole at i if its home position is not in the cyclic range (i, j].
        if (((j - home) & mask_) >= ((j - i) & mask_)) {
          slots_[i] = slots_[j];
          i = j;
        }
      }
      slots_[i] = Slot{};
      --size_;
    }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    template <class F>
    void for_each(F&& f) const {
      for (const Slot& s : slots_)
        if (s.node) f(s.id, s.node);
    }

   private:
    struct Slot {
      OrderId id = 0;
      Node* node = nullptr;
    };
    static constexpr std::size_t kInitial = 1024;
    static std::size_t hash(OrderId id) {
      return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> 20);
    }
    void place(OrderId id, Node* n) {
      std::size_t i = hash(id) & mask_;
      while (slots_[i].node) i = (i + 1) & mask_;
      slots_[i] = Slot{id, n};
    }
    void grow() {
      std::vector<Slot> old(slots_.size() * 2);
      old.swap(slots_);
      mask_ = slots_.size() - 1;
      for (const Slot& s : old)
        if (s.node) place(s.id, s.node);
    }
    std::vector<Slot> slots_;
    std::size_t mask_;
    std::size_t size_ = 0;
  };

  // Slab allocator for nodes: nodes are carved from fixed-size chunks and recycled through a free
  // list, so steady-state submit/cancel/fill never calls the system allocator.
  class NodePool {
   public:
    Node* acquire(OrderId id, Qty qty, Price price, Side side, Node* prev) {
      Node* n = free_;
      if (n) {
        free_ = n->next;
      } else {
        if (used_ == kChunk || chunks_.empty()) {
          chunks_.push_back(std::make_unique<Node[]>(kChunk));
          used_ = 0;
        }
        n = &chunks_.back()[used_++];
      }
      *n = Node{id, qty, price, side, prev, nullptr};
      return n;
    }
    void release(Node* n) {
      n->next = free_;
      free_ = n;
    }

   private:
    static constexpr std::size_t kChunk = 4096;
    std::vector<std::unique_ptr<Node[]>> chunks_;
    std::size_t used_ = 0;
    Node* free_ = nullptr;
  };
  // Price levels in one contiguous vector sorted so that the *best* level is the last element:
  // inserts and removals near the touch (where nearly all activity is) move few elements, and
  // matching walks memory linearly. begin() is the best level, like the std::map it replaced.
  template <class Cmp>
  class LevelVec {
   public:
    struct Entry {
      Price first;
      Level second;
    };
    using Vec = std::vector<Entry>;
    using iterator = typename Vec::reverse_iterator;
    using const_iterator = typename Vec::const_reverse_iterator;
    iterator begin() { return v_.rbegin(); }
    iterator end() { return v_.rend(); }
    const_iterator begin() const { return v_.rbegin(); }
    const_iterator end() const { return v_.rend(); }
    bool empty() const { return v_.empty(); }
    iterator find(Price p) {
      auto it = lower(p);
      return it != v_.end() && it->first == p ? std::make_reverse_iterator(std::next(it)) : end();
    }
    const_iterator find(Price p) const {
      auto it = lower(p);
      return it != v_.end() && it->first == p ? std::make_reverse_iterator(std::next(it)) : end();
    }
    Level& at(Price p) { return find(p)->second; }
    Level& operator[](Price p) {
      auto it = lower(p);
      if (it == v_.end() || it->first != p) it = v_.insert(it, Entry{p, Level{}});
      return it->second;
    }
    void erase(iterator r) { v_.erase(r.base() - 1); }

   private:
    // First element not ordered before p, found by scanning from the best (last) level.
    typename Vec::iterator lower(Price p) {
      auto it = v_.end();
      while (it != v_.begin() && !Cmp()(std::prev(it)->first, p)) --it;
      return it;
    }
    typename Vec::const_iterator lower(Price p) const {
      auto it = v_.end();
      while (it != v_.begin() && !Cmp()(std::prev(it)->first, p)) --it;
      return it;
    }
    Vec v_;
  };
  using BidMap = LevelVec<std::less<Price>>;     // ascending: highest bid last
  using AskMap = LevelVec<std::greater<Price>>;  // descending: lowest ask last

  void emit(const Event& e) const {
    if (handler_) handler_(e);
  }

  /// Matches a taker against the opposite side; returns the unfilled quantity.
  Qty match(OrderId id, Side side, std::optional<Price> limit, Qty qty);
  template <class Book>
  Qty match_against(Book& opp, OrderId id, Side side, std::optional<Price> limit, Qty qty);
  template <class Book>
  Qty available(const Book& opp, Side side, std::optional<Price> limit, Qty need) const;
  Qty available_for(Side side, std::optional<Price> limit, Qty need) const;
  void rest(OrderId id, Side side, Price price, Qty qty);
  /// Unlinks the node from its level (erasing the level if empty). Does not free it or touch index_.
  void unlink(Node* n);

  BidMap bids_;
  AskMap asks_;
  IdMap index_;
  NodePool pool_;
  EventHandler handler_;
};

}  // namespace lob
