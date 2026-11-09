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

## Decisions
- No exceptions for expected failures; calls return `RejectReason`.
