# queuesim: deep-dive interview questions

### 1. How do you decide whether a passive order filled?
The order joins the back of its level on arrival, with `ahead` = the level's visible shares and
`join_seq` = the book's arrival counter. Cancels and executions of orders ahead reduce `ahead`.
Once the queue in front is gone, the next execution at our price reaches us. An execution of an
order that arrived after us also reaches us (FIFO: we'd have been first), and so do executions
through our price and incoming orders that cross it. With L3 data every one of these is known
exactly. The one unknowable is how others would have reacted to our order (no-impact assumption).

### 2. Why does the naive model look *better* on markouts but not always on PnL?
- **Markouts.** Touch fills include prints at our level that never actually reached us, often
  moments when the level held and the price bounced. Those look like great fills. The queue model
  gives you fills mostly when the level is swept, i.e. when the price is moving through you.
  Across five symbols touch fills are 1.24-1.54× more numerous, and the 1 s markout is 0.13-0.24
  ¢/share better than under the queue model.
- **PnL.** It is path-dependent. More fills on a trending day (AAPL on 2019-01-30, the day after
  earnings) can mean more adverse inventory, so the naive model's PnL can be higher or lower.
  The robust conclusion is that it misstates fill rate and fill quality.

### 3. How do you guarantee no look-ahead?
- **By construction.** The strategy only ever receives View objects scheduled at
  market_ts + feed_latency, and fill reports after the same delay. Its position and open
  quantities are as-of-reports, and its orders act after the order latency.
- **By check.** Every delivery asserts view.ts + latency ≤ now (violations counted; 0 everywhere),
  and tests repeat this across latencies from 0 to 5 ms.

### 4. What bug did the tests catch?
The simulator first applied each market event to the book and only then processed pending order
arrivals with earlier timestamps. An order arriving at t therefore saw the book after an event at
t' > t. In the scripted test, the order ahead of ours had already executed, so the execution
appeared to fill us. Pending work up to t now runs before the event at t is applied. Real-data
results were regenerated after the fix.

### 5. Why does the Avellaneda–Stoikov maker lose money here?
The model assumes Poisson fills independent of the price path. Real fills are conditionally
adverse: maker markouts are negative at every horizon on every symbol, between −0.33 and −0.65 ¢
at 1 s, and fills concentrate when the price runs through the quote. The rebate ($0.0020/share)
recovers part of it, and on MSFT, INTC and CSCO the queue-model PnL is slightly positive. A
production market maker adds an adverse-selection term (short-term alpha such as order-flow
imbalance, trade-sign autocorrelation, or cross-asset signals), skews or pulls quotes ahead of
sweeps, and sizes by queue position.

### 6. Why does lower latency barely matter for this strategy?
Its quotes sit a few ticks from the mid and requote at most 10 times a second, so microseconds
don't change which queue it joins. At 5 ms, fills rise slightly (stale quotes get picked off) and
markouts worsen (−0.54 → −0.60 ¢). Latency dominates for strategies competing at the touch for
queue priority, or for takers racing on signals.

### 7. The imbalance taker loses at every threshold. Is the signal useless?
At 50 µs latency, a 5 s hold and 100 shares, yes. Taker markouts are worse than the half-spread
paid, so the top-of-book imbalance has already been priced in by the time we act. The literature
finds imbalance predictive over very short horizons, and only for participants fast enough and
with low enough costs. The backtest is doing its job: it says don't trade this.

### 8. What would you add next?
- **Queue position for icebergs/hidden:** probabilistic models when hidden liquidity
  (ITCH 'P' prints) executes at our level.
- **Market impact:** a Hawkes-style response of the order flow to our own orders, calibrated on
  data where our own orders were live.
- **Multi-symbol portfolio and cross-asset signals.**
- **Reuse with matchbox:** run strategies against the C++ matching engine with real
  queue-priority semantics.
- **A parameter-sweep grid with walk-forward splits**: the Go backtest-grid project.

### 9. Why is a Sharpe ratio from one day of one symbol meaningless?
About 380 one-minute returns, autocorrelated inventory PnL, a single regime (an earnings day), and
parameters chosen by me. The estimate's standard error is huge and multiple-testing bias is
unaccounted for. It is reported only because the output schema expects it. Credible evaluation
needs many days, walk-forward parameter selection, and deflated Sharpe ratios.
