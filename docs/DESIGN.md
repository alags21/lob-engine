# Design

Design notes, written stage by stage and consolidated at the end: matching semantics, verification,
performance, concurrency, replay and visualizer, then tradeoffs/rejected alternatives, invariants and
limitations. Start with [Limitations](#limitations) before quoting any number.

## Layout
- `include/lob/`: public headers (order book, types, event log, SPSC and mutex queues, checksum, market simulator, replay IO).
- `src/`: library implementation (`lob` target).
- `tests/`: GoogleTest unit, stress, pipeline, fuzz and simulator tests; `tests/reference/` is the reference model.
- `bench/`: single-thread (`lob_bench`) and multi-threaded (`lob_mt_bench`) benchmark harnesses, workloads, saved results.
- `tools/`: `lob_replay`, `lob_sim`, `lob_cli`.
- `viz/`: the static visualizer; `scripts/`: build, benchmark, report and data-generation scripts.

## Build options
`LOB_BUILD_TESTS`, `LOB_BUILD_BENCH`, `LOB_BUILD_TOOLS`, `LOB_SANITIZE` (ASan+UBSan), `LOB_TSAN`, `LOB_RACY_DEMO` (tests only, off by default), `LOB_WERROR`.

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
- **Storage as first written (deliberately naive; replaced in Stage 3, see below)**: `std::map<Price, Level>` per side
  (bids `greater`), each level a `std::list` of resting orders plus a cached total, and an
  `unordered_map<OrderId, Locator>` holding a list iterator for O(log levels) cancel. Chosen for
  obvious correctness; it is the baseline of the Stage 3 benchmarks.
- **Not thread-safe**; concurrency is handled outside the book (Stage 4).

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
  committed), the fuzzer reported a mismatch immediately (the failure message names the op index and seed).
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

- `passive`: SPSC is 1.64x the mutex+condvar queue (a win) and 0.63x the mutex-protected book (a LOSS: the lock-only design with no hand-off is faster); it reaches 49% of the single-thread ceiling. Paced median latency 355 ns vs 970 ns (mutex+condvar) and 226 ns (mutex-book).
- `aggressive`: SPSC is 1.79x the mutex+condvar queue (a win) and 0.63x the mutex-protected book (a LOSS: the lock-only design with no hand-off is faster); it reaches 47% of the single-thread ceiling. Paced median latency 323 ns vs 980 ns (mutex+condvar) and 198 ns (mutex-book).
- `balanced`: SPSC is 1.68x the mutex+condvar queue (a win) and 0.64x the mutex-protected book (a LOSS: the lock-only design with no hand-off is faster); it reaches 52% of the single-thread ceiling. Paced median latency 353 ns vs 988 ns (mutex+condvar) and 222 ns (mutex-book).

Tail latency (p99 and above) on this machine is dominated by OS interference; see docs/CONCURRENCY.md.
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

## Stage 5: replay and visualizer
- **Synthetic market** (`include/lob/market_sim.hpp`): Poisson arrivals, a hidden fundamental price
  doing a +-1 tick walk, passive quotes placed around it with a configurable depth distribution
  (geometric, uniform, two-scale fat tail), configurable cancel/modify fractions and aggressiveness
  (market, IOC, FOK and marketable GTC orders). Cancels and modifies read the book, so they always
  target live orders. Only synthetic data is used; nothing is downloaded.
- **Determinism**: the generator uses only integer arithmetic and IEEE `+ - * /` (its own series
  `log()` for the exponential gaps, no libm), so a config produces the identical run on every
  platform. A unit test pins the checksum of one config, and CI regenerates the committed sample on
  Linux and fails if it differs from the file generated on Windows.
- **Recorder / export**: `Recorder` applies timed ops to the (optimised) book and records every
  event plus a book snapshot every K ops (top N levels per side). `lob_replay` reads the text op log,
  CSV or JSON lines and exports the same `lob-run-v1` JSON, so a recorded stream can be replayed and
  visualised. Formats: `docs/EVENT_FORMAT.md`. The golden-log replay test still passes unchanged.
- **Visualizer** (`viz/`): static HTML + canvas + plain JS, no build step, no CDN, no network
  access. It shows an animated depth chart, trade tape, mid price with trades and spread over time,
  order-flow stats and mix, and a latency histogram. Playback: play/pause, step (one snapshot),
  speed, scrub, keyboard. The sample run and the latency data are loaded through `<script>` tags so
  the page also works from `file://`; "Load run" opens other exports. The latency panel shows only
  data copied from `bench/results/*.json` by `scripts/make_viz_data.py` (Stage 3 book steps and
  Stage 4 queue variants) and is labelled as independent of the simulated run.
- **Testing**: `viz/test/viz_test.js` (Node, no dependencies) loads the real sample and latency data,
  checks the derived model against the counts in the export, drives every draw function at many
  cursor positions against a mock canvas and fails on any NaN/Infinity coordinate, exercises the DOM
  wiring (play, pause, step, scrub, speed, keyboard, selectors) against a fake DOM, and checks that
  the page references only local files and no network URLs. A deliberately broken draw call makes it
  fail. Pixels and layout are checked separately by screenshotting the page in a headless
  Chromium-based browser (`scripts/screenshot_viz.py`, output `docs/img/viz-sample.png`); that was
  done on Windows with Edge only. Not tested: other browsers, touch input, narrow screens.
- **Pages**: `.github/workflows/pages.yml` can deploy `viz/` to GitHub Pages but is manual-trigger only
  and Pages is not enabled by this repository; the owner has to enable it in the settings first.

## Tradeoffs and what was rejected

Each Stage 3 change was measured on three workloads and kept only if it helped; the table in
[PERFORMANCE.md](PERFORMANCE.md) has the numbers. What follows is the reasoning, not new measurements.

- **Intrusive order lists (step 1)**: on its own this was *not* a measurable win (within run-to-run
  noise, and slightly slower on some workloads). It was kept anyway because the node pool needs
  nodes that carry their own links, and because it removes a per-order allocation indirection. Had
  it been judged purely on its own row it would have been dropped; that is called out in PROGRESS.
- **Node pool (step 2)** helped modestly: it removes the system allocator from the hot path.
- **Flat open-addressing id table (step 3)** was the largest single win: the standard
  `std::unordered_map` allocates a node per order and chases pointers on every lookup.
- **Contiguous sorted-vector price levels (step 4)** were kept; a *binary search* for the level helped
  on some workloads and was within noise on others.
- **Scan from the best level (step 5)** beat binary search because activity clusters at the touch.
  The tradeoff is real: a book whose orders sit far from the touch would favour binary search, and
  only three bundled workloads were measured.
- **Not tried or not kept**: a dense array indexed by price (rejected: the price range is unbounded
  and sparse levels waste memory; sorted vector keeps the code general); replacing the
  `std::function` event callback with a template parameter (rejected to keep the public API stable and
  because its cost is a constant included in every measurement); batching or prefetching in the
  matcher (not attempted).
- **Queue designs (Stage 4)**: the lock-free ring beat a mutex+condvar queue but **lost** to simply
  locking the book (no hand-off, uncontended lock) and cannot exceed the single-thread ceiling; its
  value is decoupling the producer from matching, not speed. A cache-line-per-slot variant and a
  yield wait strategy each helped one metric (throughput) and were not made the default because they
  did not help, or hurt, the metric the default was chosen for (paced median latency).
  Unpadded indices were measurably slower, so padding stays.
- **Multi-producer / MPMC queue**: not built. One producer keeps the ordering argument short and
  makes "same events as the single-thread run" checkable by checksum.
- **Self-trade prevention, order persistence, market-data adapters**: not built. The book has no
  notion of participant and no recorded data is used.
- **Exceptions for rejects**: not used; calls return a reason code and emit a Rejected event, which
  keeps the hot path allocation-free and the event stream complete.

## Invariants

`OrderBook::validate()` returns the first violated invariant, and in builds without `NDEBUG` every
mutating call runs it and aborts on failure (so the whole unit-test and fuzz suites run with it):

- no crossed or locked book (best bid < best ask);
- no empty price level, no non-positive resting quantity, no non-positive price;
- every level's cached total and order count equal the sum over its orders;
- each level's order list is well formed (head/tail terminated, `prev`/`next` consistent) and every
  node's side and price match the level that holds it;
- the id index contains exactly the resting orders, each pointing at its node.

FIFO within a price level cannot be derived from state alone, so it is covered by the differential
fuzzer, which compares every level's queue order against the reference model after each check.
Separately, the reference model is trusted only as far as its author's reading of the spec; the
hand-checked golden log is the independent check.

## Event semantics (summary)

Defined in the Stage 1 section above and in [EVENT_FORMAT.md](EVENT_FORMAT.md): trades execute at the
resting order's price; FIFO within a level; an order that crosses an earlier order always trades
(no self-trade prevention); for a new order the stream is Accepted, then Trades, then at most one
Cancelled (IOC/market remainder or FOK kill); a modify emits Modified then any Trades, and only a
same-price size reduction keeps queue priority; failures emit Rejected instead of Accepted. Events
are delivered synchronously in occurrence order, which is what makes the replay and the
multi-threaded run reproducible byte for byte.

## Limitations

Read these before quoting any number from this repository.

- **One machine, Windows timings only.** Every timing comes from a single Windows desktop. Linux is
  exercised in CI for correctness and sanitizers, but no Linux timing exists, and CI timings are
  never used because the runners are shared and noisy.
- **Desktop noise.** The machine was not isolated: normal background load, turbo/boost uncontrolled,
  and a CPU with two core classes (Stage 3 was pinned to the lower class; a re-run on the higher
  class is reported). Run-to-run drift was measured and is reported next to the results; ratios
  between steps are the result, absolute ops/s are not portable. Tail latencies above the median are
  dominated by OS interference in the multi-threaded runs.
- **Closed-loop and paced-loop assumptions.** Stage 3 is closed loop: each op starts when the
  previous one returns, so there is no queueing and the numbers are service times, not behaviour
  under an arrival process. Stage 4 latency uses a paced producer measured from the intended send
  time (which avoids coordinated omission), but at one fixed rate and with one producer.
- **Timer cost.** Per-op latency uses a serialised TSC read whose pair cost is recorded in every
  result file and is not subtracted.
- **Cross-core TSC.** Cross-thread latency compares TSC values on two cores; the CPUID invariant bit
  was checked and the offset estimated by ping-pong, which assumes symmetric delay.
- **No multi-producer** support, no persistence, no recovery, no network or market-data adapters. The
  book is not thread-safe; the threaded pipeline runs it on a single matching thread.
- **Synthetic data only.** All workloads and the visualizer sample are generated; results say nothing
  about real market microstructure.
- **Reference model and book share an author** (see Invariants). Fuzz prices and quantities are
  confined to narrow ranges.
- **ThreadSanitizer** runs only in Linux CI (unavailable with MSVC). A deliberately racy queue build
  confirms TSan can fail on this code; passing TSan is evidence, not proof, of freedom from races.
- **Visualizer.** Screenshot-checked in one browser (headless Edge, dark theme, desktop width) and
  opened once in a browser pane by the project owner's assistant; automated checks use a mock canvas
  and a fake DOM and do not check pixels. Not tested: Firefox, Safari, the light theme, narrow or touch
  screens. The book animates at snapshot granularity.
- **Not yet merged or released.** The work is a stack of pull requests; see [RELEASE.md](RELEASE.md).
  No license file has been added.

## Decisions
- No exceptions for expected failures; calls return `RejectReason`.
