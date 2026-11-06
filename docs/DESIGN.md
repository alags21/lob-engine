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

## Stage 4: concurrency design
Topology: one producer thread feeds a bounded queue; one matching thread owns the `OrderBook` and
applies ops in FIFO order. The book itself stays single-threaded and lock-free; all synchronisation
is in the queue. Because there is one producer and the queue is FIFO, the matching thread sees
exactly the producer's op sequence, so the event stream and final book must equal the single-thread
run. This is enforced: the benchmark checksums every queue variant against the single-thread run
before timing and exits non-zero on a mismatch, and the unit tests do the same for many seeds and
capacities. Results (generated):

<!-- BEGIN GENERATED:mt -->
Stage 4, producer thread -> queue -> matching thread (medians of repeated runs; latency measured from the intended send time at a fixed paced rate; see docs/CONCURRENCY.md for method, ablations and caveats).

| workload | single-thread Mops/s | SPSC Mops/s | mutex+condvar Mops/s | mutex-book Mops/s | SPSC paced p99 ns | mutex+condvar paced p99 ns | mutex-book paced p99 ns |
|---|---|---|---|---|---|---|---|
| passive | 11.15 | 5.42 | 3.32 | 8.60 | 364113 | 390838 | 175498 |
| aggressive | 12.85 | 6.05 | 3.39 | 9.64 | 371021 | 463838 | 167277 |
| balanced | 9.95 | 5.22 | 3.11 | 8.19 | 412628 | 1893520 | 148974 |

- `passive`: SPSC beats the mutex+condvar queue on throughput (1.64x); does NOT beat the mutex-protected book on throughput (0.63x); reaches 49% of the single-thread ceiling. Paced latency, median: p50 355 ns vs 970 ns for mutex+condvar; p50 355 ns vs 226 ns for mutex-book; p50 355 ns vs 198 ns for single-thread (no hand-off).
  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 187261 ns and was behind schedule for 2.8% of ops. Paced p99 of the other rows: spsc 364113 ns, mutex+condvar 390838 ns, mutex-book 175498 ns; differences at that level are not reliable evidence about the queues.
  - one-slot-per-cache-line variant: 6.02 vs 5.42 Mops/s (1.11x; CV 4.9% / 5.6%).
  - padding ablation: padded 5.42 vs unpadded 4.86 Mops/s (1.12x); padding helped (CV 5.6% / 3.3%).
  - wait strategy throughput: spsc 5.42, spsc-yield 8.13, spsc-backoff 5.70 Mops/s; paced p99: spsc 364113 ns, spsc-yield 1348150 ns, spsc-backoff 1341790 ns.
- `aggressive`: SPSC beats the mutex+condvar queue on throughput (1.79x); does NOT beat the mutex-protected book on throughput (0.63x); reaches 47% of the single-thread ceiling. Paced latency, median: p50 323 ns vs 980 ns for mutex+condvar; p50 323 ns vs 198 ns for mutex-book; p50 323 ns vs 164 ns for single-thread (no hand-off).
  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 113359 ns and was behind schedule for 2.4% of ops. Paced p99 of the other rows: spsc 371021 ns, mutex+condvar 463838 ns, mutex-book 167277 ns; differences at that level are not reliable evidence about the queues.
  - one-slot-per-cache-line variant: 6.60 vs 6.05 Mops/s (1.09x; CV 4.1% / 2.0%).
  - padding ablation: padded 6.05 vs unpadded 5.16 Mops/s (1.17x); padding helped (CV 2.0% / 6.4%).
  - wait strategy throughput: spsc 6.05, spsc-yield 8.01, spsc-backoff 6.09 Mops/s; paced p99: spsc 371021 ns, spsc-yield 2154130 ns, spsc-backoff 441745 ns.
- `balanced`: SPSC beats the mutex+condvar queue on throughput (1.68x); does NOT beat the mutex-protected book on throughput (0.64x); reaches 52% of the single-thread ceiling. Paced latency, median: p50 353 ns vs 988 ns for mutex+condvar; p50 353 ns vs 222 ns for mutex-book; p50 353 ns vs 186 ns for single-thread (no hand-off).
  - tail latency is dominated by OS interference on this machine: the no-queue single-thread row has p99 141177 ns and was behind schedule for 2.9% of ops. Paced p99 of the other rows: spsc 412628 ns, mutex+condvar 1893520 ns, mutex-book 148974 ns; differences at that level are not reliable evidence about the queues.
  - one-slot-per-cache-line variant: 5.89 vs 5.22 Mops/s (1.13x; CV 2.7% / 2.5%).
  - padding ablation: padded 5.22 vs unpadded 4.66 Mops/s (1.12x); padding helped (CV 2.5% / 3.6%).
  - wait strategy throughput: spsc 5.22, spsc-yield 7.01, spsc-backoff 5.32 Mops/s; paced p99: spsc 412628 ns, spsc-yield 2507160 ns, spsc-backoff 451755 ns.
<!-- END GENERATED:mt -->

**SPSC ring (`include/lob/spsc_queue.hpp`)**: power-of-two capacity, free-running 64-bit indices,
slot = index & mask. Orderings (argued in full in the header):
- `tail_` store: `release` by the producer after writing the slot; `acquire` load by the consumer, so
  the slot write happens-before the consumer's read (no torn item).
- `head_` store: `release` by the consumer after reading the slot; `acquire` load by the producer, so
  the slot is not overwritten while still being read.
- Each side reads its own index `relaxed` (single writer).
- `cached_head_` / `cached_tail_` are private, non-atomic, lower bounds of the other side's progress;
  the shared line is re-read (with acquire) only when the cache says full/empty.
- No `seq_cst` is needed: there is no property spanning more than the two hand-offs above.
Padding: producer-owned (`tail_`, `cached_head_`) and consumer-owned (`head_`, `cached_tail_`) fields
sit on separate 64-byte lines; the unpadded variant is kept only as a benchmark ablation.
Slots themselves are contiguous, so adjacent slots can share a line; this was not changed.

**Wait strategy** (what to do when the queue is full/empty): the default is a pure spin with the
x86 `pause` hint (`spsc`), because both threads are pinned to dedicated cores and the point is
low hand-off latency. `spsc-yield` (yield to the scheduler) and `spsc-backoff` (spin 128 times,
then yield) are measured as alternatives; the measured comparison is in `docs/CONCURRENCY.md`.
Spinning burns a core and is the wrong choice when cores are shared or oversubscribed.

**Baselines**: `mutex-cv-queue` (bounded `std::mutex` + two condition variables, notifying only on
empty->non-empty and full->not-full), `mutex-book` (the producer thread locks a mutex around each
`apply`, no hand-off: this isolates uncontended lock cost and is not a contended-lock test), and
`single-thread` (the Stage 3 book, the ceiling).

**Latency method**: the producer runs on a fixed schedule; latency = matching-thread completion TSC
minus the *intended* send TSC, so a producer stalled by a full queue is charged for it (no
coordinated omission). Both timestamps use the same invariant TSC read on two pinned cores; the
benchmark checks the CPUID invariant-TSC bit and estimates the inter-core offset with a ping-pong.
Throughput is measured separately with an unpaced producer.

**ThreadSanitizer**: CI has a TSan job (clang, Linux) running the queue, stress and pipeline tests
and a small run of the multi-threaded benchmark. TSan is not available with MSVC, so it has never
been run on the Windows development machine; Windows only runs the plain tests and benchmarks.

## Decisions
- No exceptions for expected failures; calls return `RejectReason`.
