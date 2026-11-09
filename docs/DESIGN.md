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

## Decisions
- No exceptions for expected failures; calls return `RejectReason`.
