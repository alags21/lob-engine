# Design

Skeleton. Filled in per stage; final tradeoffs in Stage 6.

## Layout
- `include/lob/`: public headers.
- `src/`: library implementation (`lob` target).
- `tests/`: GoogleTest unit tests.
- `bench/`: Google Benchmark targets (placeholder until Stage 3).
- `tools/`: command-line tools (placeholder).

## Build options
`LOB_BUILD_TESTS`, `LOB_BUILD_BENCH`, `LOB_BUILD_TOOLS`, `LOB_SANITIZE` (ASan+UBSan), `LOB_WERROR`.

## Stage 1: matching semantics
- **Units**: price in integer ticks (`int64`, must be > 0), quantity in integer lots (`int64`, > 0).
- **Priority**: best price first, then FIFO by arrival within a level. Trades print at the maker's
  resting price, so price improvement goes to the taker.
- **Order types**: limit (GTC rests; IOC cancels remainder; FOK is all-or-nothing, checked against
  crossing liquidity before any trade happens), market (any price, never rests, remainder
  cancelled; FOK market also supported). The `price` of a market order is ignored.
- **Cancel**: only live orders. Cancelling a filled/cancelled/unknown id emits Rejected(UnknownOrder).
- **Modify** takes the new *remaining* quantity and price. Same price and smaller or equal size is
  amended in place and keeps queue position. A price change or a size increase is cancel/replace:
  the order moves to the back of its (new) level and, if it now crosses, trades as a taker. This
  follows the common exchange rule that only size reductions preserve priority.
- **Ids**: must be unique among *live* orders; an id may be reused after fill/cancel. The book does
  not remember dead ids.
- **Self-trade**: no participant identity, so no prevention. An order crossing an earlier resting
  order always trades with it, regardless of who submitted it.
- **Events**: delivered synchronously through a callback in occurrence order. For a new order:
  Accepted, then Trades, then Cancelled for any IOC/market remainder or FOK kill. Rejected is
  emitted for failures instead of Accepted. Trade carries taker id, maker id, taker side, maker
  price, quantity.
- **Storage (deliberately naive, replaced in Stage 3)**: `std::map<Price, Level>` per side
  (bids `greater`), each level a `std::list` of resting orders plus a cached total, and an
  `unordered_map<OrderId, Locator>` holding a list iterator for O(log levels) cancel. Chosen for
  obvious correctness; the Stage 3 benchmark will use it as the baseline.
- **Not thread-safe**; concurrency arrives in Stage 4.

## Stage 2: verification design
- **Reference model** (`tests/reference/`): `std::map<Price, std::deque>` per side, no id index and
  no cached totals (cancel/modify scan every level). Written from the semantics above rather than
  copied from `OrderBook`, so a shared misunderstanding is still possible; it is an oracle for
  implementation bugs, not for the spec.
- **Differential fuzzing** (`tests/fuzz/`): a splitmix64-seeded generator (same stream on every
  platform; no `std::` distributions) feeds both books. Every op compares the returned status and
  the full emitted event sequence; every `--check-every` ops (1 in unit tests/Debug/ASan, 16 in the
  long runs) it also compares all levels and every level's queue (so FIFO order) and runs
  `OrderBook::validate()`. The stream mixes GTC/IOC/FOK limits, markets (with ignored prices),
  cancels and modifies (mostly of open orders, some stale/unknown), and invalid or duplicate-id
  submits around a drifting mid so the book crosses often. A pool-size thermostat keeps the book at
  hundreds to a few thousand orders so the O(n) reference stays tractable.
- **Invariants** (`OrderBook::validate()`): no crossed/locked book, no empty level, no non-positive
  quantity, level total equals the sum of its orders, id index matches the levels exactly (side,
  price, iterator). In builds without `NDEBUG` every public mutating call runs it and aborts on
  failure. FIFO cannot be derived from state alone, so it is covered by the differential queue
  comparison.
- **Fuzzer sanity check**: with a deliberately broken FOK pre-check applied locally (not
  committed), the fuzzer reported a mismatch at op 242 of seed 1.
- **Op log / event format**: see `docs/EVENT_FORMAT.md`. Text, one record per line, so logs diff
  cleanly and failing fuzz runs are replayable with `lob_replay`.

## Stage 3: performance design
Process: build the harness, record the naive Stage 1 book as the baseline, then change one thing per
commit, run the Stage 2 correctness gate (unit tests, differential fuzz against the reference model,
`validate()` after every call), benchmark, keep the change only if it measurably helps. Per-step
numbers: [PERFORMANCE.md](PERFORMANCE.md) (generated). Headline (generated):

<!-- BEGIN GENERATED:perf -->
Baseline `00-baseline` vs `05-linear-scan-levels` (medians of repeated runs; see docs/PERFORMANCE.md for method, variance and every step).

| workload | baseline Mops/s | final Mops/s | speedup | baseline p99 ns | final p99 ns | baseline p99.9 ns | final p99.9 ns | allocs/op before -> after |
|---|---|---|---|---|---|---|---|---|
| passive | 4.79 | 11.53 | 2.41x | 631 | 281 | 1483 | 521 | 0.864 -> 0.000 |
| aggressive | 5.79 | 12.88 | 2.22x | 701 | 301 | 1573 | 551 | 0.764 -> 0.000 |
| balanced | 4.66 | 9.32 | 2.00x | 741 | 381 | 1723 | 771 | 0.811 -> 0.000 |
<!-- END GENERATED:perf -->

Data structures now (public API unchanged apart from `OrderBook` being non-copyable, which it never
could safely be):
- **Orders** are intrusive doubly linked nodes (`prev`/`next` in the node), one list per price level,
  front = highest time priority. Cancel and in-place amend are O(1) once the node is found.
- **Node storage** comes from a slab allocator with a free list (4096-node chunks); after warm-up no
  submit, fill or cancel touches the system allocator.
- **Id lookup** is an open-addressing table (linear probing, power-of-two capacity, load <= 1/2,
  Fibonacci hashing, backward-shift deletion so there are no tombstones).
- **Price levels** per side are one contiguous `std::vector` sorted so the best level is the last
  element; insert/erase near the touch move few elements. Levels are located by scanning from the
  best level rather than by binary search, because activity clusters at the touch. That choice is
  workload dependent: a very deep book with orders far from the touch would favour binary search.
- **Allocation on the hot path**: `lob_bench` replaces global `operator new` with a counting
  wrapper and reports allocations per op; the remaining allocations are one-off growth of the slab
  list, the id table and the level vectors.
- Events still go through a `std::function` callback (part of the public API), so its cost is
  included in every measurement.

## Decisions
- No exceptions for expected failures; calls return `RejectReason`.
