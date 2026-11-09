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
