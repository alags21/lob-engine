# Op log and event formats (v1)

Plain text, one record per line, fields separated by single spaces, `\n` line endings (a trailing
`\r` is ignored on read). Blank lines and lines starting with `#` are comments. All integers are
decimal. The encoder/decoder is `include/lob/event_log.hpp` (`to_string`, `parse_op`,
`parse_event`).

Determinism: the book is single-threaded and has no clock or randomness, so replaying the same op
log yields a byte-identical event stream on every platform and compiler. `tests/data/golden.log`
and `tests/data/golden.events` pin this (the expected events were checked by hand and are replayed
through both `OrderBook` and the reference model by `EventLog.GoldenReplayMatchesCommittedEvents`).

## Op log (input)

| Line | Meaning |
|---|---|
| `S <id> <B\|S> <L\|M> <GTC\|IOC\|FOK> <price> <qty>` | submit. `L` limit, `M` market (price ignored, write `0`) |
| `C <id>` | cancel |
| `M <id> <price> <qty>` | modify to new price and new remaining quantity |

Ops are applied in file order. Invalid values (zero/negative qty or price, duplicate or unknown
id) are legal in a log; they produce `R` events. Failing fuzz runs write their op log in this format.

## Event stream (output)

| Line | Event | Fields |
|---|---|---|
| `A <id> <B\|S> <price> <qty>` | Accepted | price is `0` for a market order |
| `R <id> <reason>` | Rejected | reason: `DUPLICATE_ID`, `INVALID_QTY`, `INVALID_PRICE`, `UNKNOWN_ORDER` |
| `T <taker> <maker> <B\|S> <price> <qty>` | Trade | side is the taker's; price is the maker's resting price |
| `X <id> <B\|S> <price> <qty> <reason>` | Cancelled | reason: `USER`, `IOC`, `MARKET`, `FOK`; price is the resting price for `USER`, else `0`; qty is the quantity removed |
| `D <id> <B\|S> <price> <qty> <0\|1>` | Modified | new price and qty; last field is 1 if queue priority was lost |

Events for one op appear in occurrence order: `A`, then `T`s, then at most one `X`; a modify
emits `D` then `T`s.

## Replay

```
lob_replay tests/data/golden.log > out.events
diff out.events tests/data/golden.events
```

The fuzzer's failure output prints the seed and op index; the written `fuzz-fail-seed<S>.log` is a
valid op log and can be fed straight to `lob_replay`.

## Timed op streams (CSV, JSON lines)

Used by the synthetic market generator and by `lob_replay`. Same ops as the text op log, plus a
timestamp in nanoseconds (strictly increasing in generated runs).

CSV, header `t_ns,op,id,side,type,tif,price,qty`, one op per line:

```
t_ns,op,id,side,type,tif,price,qty
1822,S,1,B,L,GTC,9994,12
2450,C,1,,,,,
9100,M,7,,,,9995,4
```

`op` is `S` (submit), `C` (cancel) or `M` (modify). `side` is `B`/`S`, `type` is `L`/`M`, `tif` is
`GTC`/`IOC`/`FOK`; these three are empty for cancel and modify, and `price`/`qty` are empty for
cancel. JSON lines carry the same fields as flat objects, for example
`{"t":1822,"op":"S","id":1,"side":"B","type":"L","tif":"GTC","price":9994,"qty":12}`
(`side`, `type`, `tif` omitted for `C` and `M`; `price` and `qty` omitted for `C`).
Lines starting with `#` and blank lines are ignored.

```
lob_sim --seed 7 --ops 8000 --csv run.csv --jsonl run.jsonl        # generate and export ops
lob_replay run.csv                                                 # print events
lob_replay --json run.json --snapshot-every 8 --levels 15 run.csv  # replay and export a run
```

## Run export (`lob-run-v1`, JSON) for the visualizer

Written by `lob_sim --json/--js` and `lob_replay --json/--js`. All numbers are integers except the
generator fractions in `meta`. The `--js` variant is the same JSON assigned to `window.LOB_RUN`, so
a page can load it with a `<script>` tag from `file://`.

- `meta`: `source` (`simulated` or `replay`), generator config (seed, arrival rate, cancel and modify
  fractions, aggressiveness, depth distribution, ...), `snapshot_every`, `depth_levels`,
  `duration_ns`, `counts` (submits, cancels, modifies, events, trades, rejects) and `checksum`
  (hash of every event and the final book, `include/lob/checksum.hpp`).
- `ops`: `[t_ns, kind, id, side, type, tif, price, qty]` with kind 0/1/2 = submit/cancel/modify, side
  0/1 = buy/sell, type 0/1 = limit/market, tif 0/1/2 = GTC/IOC/FOK.
- `events`: `[op_index, type, order_id, maker_id, side, price, qty, aux]`, type 0..4 = accepted,
  rejected, trade, cancelled, modified; `aux` is the reject reason, cancel reason or lost-priority flag.
  For a trade, `order_id` is the taker, `side` the taker's side and `price` the maker's price.
- `snapshots`: `[op_index, t_ns, bids, asks]`, state after `op_index` ops, each side a best-first list
  of `[price, qty, orders]` limited to `depth_levels` levels. The first snapshot is op 0 (empty
  book) and the last is the final state.
