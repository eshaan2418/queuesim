# queuesim design

## Event loop
```
ITCH stream (gzip, 64 MiB chunks) ─ filter by stock locate ─▶ Simulator per fill model
                                                              │
   market event at ts ──▶ advance(ts): run every pending item due ≤ ts, in time order
                          (view deliveries, order arrivals, cancels, fill reports, markouts)
                     ──▶ apply event to the true L3 book
                     ──▶ queue model: update virtual orders (ahead counts, fills)
                     ──▶ if the top of book changed: schedule a View for ts + feed latency
```
- Pending work is a min-heap keyed by (time, insertion sequence), so ties are FIFO and
  deterministic.
- **Ordering rule:** pending work up to `ts` runs **before** the event at `ts` is applied. An
  order that arrives at t must see the book as of t. A test caught the opposite ordering, which
  let an arriving order see a later event already applied: the order ahead of it had vanished,
  and its execution then filled us.
- All three fill models run in one pass over the data (one simulator and strategy instance each).
  A full-day run takes 15-28 s, dominated by single-threaded zlib decompression of 11 GB.

## Information barriers (no look-ahead)
- The strategy receives only `View`s, each scheduled at `market_ts + feed_latency`. Every delivery
  checks `view.ts + feed_latency ≤ now` and counts violations (0 across every run and test).
- `Ctx::position()` and `open_qty()` reflect only fill reports delivered so far (themselves
  delayed by the feed latency), not the true position.
- Orders and cancels take effect at `now + order_latency`.

## L3 book with arrival sequence
Each order gets a monotonically increasing arrival number. A virtual order records `join_seq` (the
counter at its arrival) and `ahead` (the level's visible shares at arrival). For an event at our
side and price:
- **Remove (cancel/delete/replace-out)** of an order with `seq ≤ join_seq`: `ahead -= shares`.
- **Execute** of an order with `seq ≤ join_seq`: it consumes `ahead`, and any excess reaches us.
- **Execute** of an order with `seq > join_seq` (behind us): FIFO says we would have traded first,
  so it fills us.
- **Execute** at a price worse than ours (trade-through), or an opposite-side **add** that crosses
  our price: it would have traded with us, so it fills us.

Replaces are a remove plus an add (new arrival number), matching ITCH semantics (priority is
lost).

## Accounting
Cash in dollars, position in shares, maker rebate / taker fee per share. Mark-to-mid at the end of
the window. A 1-minute equity curve feeds the Sharpe ratio, which is reported but not trusted (one
day, one symbol). Markouts at 1 s, 5 s and 30 s are scheduled per fill, signed so positive is
favourable, and volume-weighted.

## Not modelled
- Market impact of our own orders (virtual orders do not change the real book or other
  participants' behaviour).
- Hidden or reserve liquidity (only displayed L3 is observable).
- Auctions.
- Exchange-side order throttles and the round trip for cancel acknowledgements (cancels take
  effect after the order latency).
