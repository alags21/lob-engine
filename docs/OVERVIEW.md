# Overview

One-page map of the project: what it is, how it is built, what it can and cannot do, and which
tradeoffs were made. It deliberately contains **no benchmark numbers**; every measured figure lives in
a generated table (see [RESULTS.md](RESULTS.md)) next to its method and caveats. Read
[Limitations](DESIGN.md#limitations) before quoting any of them.

## What it is
A C++20 limit order book and matching engine, built stage by stage with a correctness-first test
strategy. It is a library (`lob`) plus tools, tests, benchmarks and a browser visualizer. It is not a
trading system: no network, persistence, participant identity or live market-data adapters.

## Architecture
```
 lob_sim (seeded market) ─┐
 CSV / JSONL / oplog ─────┴─> lob_replay ─┐
 Producer thread -> SPSC ring -> Matching thread ─┤
                                                  v
                        OrderBook  (single-threaded, deterministic)
                          ├─ price levels: sorted contiguous vector per side (best = last)
                          ├─ orders: intrusive FIFO lists, slab-pool nodes
                          └─ id index: open-addressing hash table
                                                  │ synchronous event callback
                                                  v
                 Events (Accepted / Trade / Cancelled / Modified / Rejected)
                     ├─ checksum / text event log (replayable, diffable)
                     └─ lob-run-v1 JSON ─> viz/ static page
 Reference model + differential fuzzer + validate() check the book independently.
```
Key property: the book is single-threaded and has no clock or randomness, so the same ops always
produce the same events on every platform. Concurrency lives outside it (one producer, one lock-free
queue, one matching thread) and is verified to yield an event stream identical to the single-thread run.

## Features
| Area | What exists | Details |
|---|---|---|
| Matching | Limit (GTC/IOC/FOK), market, cancel, modify; price-time priority; trades at maker price | [DESIGN.md](DESIGN.md#stage-1-matching-semantics) |
| Verification | Slow reference model, seeded differential fuzzer, `validate()` invariants, golden log | [DESIGN.md](DESIGN.md#stage-2-verification-design) |
| Performance | Five measured optimisation steps, zero hot-path allocations after warm-up | [PERFORMANCE.md](PERFORMANCE.md) |
| Concurrency | SPSC ring (acquire/release, padded), mutex baselines, checksum gate, TSan CI | [CONCURRENCY.md](CONCURRENCY.md) |
| Replay / sim | Seeded synthetic market, CSV/JSONL/oplog replay, `lob-run-v1` export | [EVENT_FORMAT.md](EVENT_FORMAT.md) |
| Visualizer | Static offline page: depth chart, tape, mid/spread, flow stats, latency histogram | `viz/` |
| Real data | One Nasdaq ITCH 5.0 day replayed through the engine (data not included) | [REALDATA.md](REALDATA.md) |

## Matching rules in brief
- Integer ticks and lots. Best price first, then FIFO within a level; trades print at the resting price.
- Only a same-price size reduction keeps queue priority; price change or size increase is cancel/replace.
- Ids are unique among live orders only. No self-trade prevention (no participant identity).
- Per order the events are Accepted, then Trades, then at most one Cancelled; failures emit Rejected.

## Main tradeoffs
- **Correctness before speed:** every optimisation was a separate commit gated by unit tests, the
  differential fuzz and per-call invariants, and kept only if it measurably helped.
- **Scan-from-best level lookup** beat binary search because activity clusters at the touch; a deep
  book with far-from-touch orders would favour binary search.
- **`std::function` event callback** keeps the API simple but its cost is in every measurement.
- **Lock-free SPSC queue** beats a mutex+condvar queue but loses to simply locking the book and cannot
  exceed the single-thread ceiling; its value is decoupling, not speed.
- **Single producer only** keeps the ordering argument short and checkable by checksum.
- **Rejected alternatives:** dense price-indexed array, MPMC queue, exceptions for rejects
  (see [DESIGN.md](DESIGN.md#tradeoffs-and-what-was-rejected)).

## Known gaps (summary)
Timings come from one non-isolated Windows desktop (ratios, not absolute ops/s, are the result); no
Linux timings; the reference model and book share an author and spec; fuzz ranges are narrow; real-data
validation covers one day of one venue; the visualizer was checked in one browser. Full list:
[RESULTS.md](RESULTS.md#known-gaps).

## Where to go next
| I want to... | Read |
|---|---|
| Build, run, verify | [README](../README.md#quick-start), [CLAUDE.md](../CLAUDE.md) |
| Understand a design decision | [DESIGN.md](DESIGN.md) |
| See what each stage delivered | [RESULTS.md](RESULTS.md) |
| Reproduce benchmarks | [BENCHMARKING.md](BENCHMARKING.md), [PLATFORMS.md](PLATFORMS.md) |
| Parse or write logs | [EVENT_FORMAT.md](EVENT_FORMAT.md) |
