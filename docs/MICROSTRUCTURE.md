# Market microstructure behind queuesim

## Why queue position is the whole game for a passive strategy
On a price-time priority venue, a resting limit order at the best bid executes only after every
order ahead of it at that price has executed or been cancelled. Whether a market maker's quote
earns the spread or gets run over depends on **where it sits in the queue** and **why** the queue
in front of it disappeared:
- The queue ahead **cancels**: our order moves up, and that is good, because the queue was
  reduced by participants changing their minds rather than by an informed sweep.
- The queue ahead **trades away**: that is aggressive flow hitting the level. If it eats through
  the whole queue and reaches us, it is likely informed flow that will push the price further.
  This is adverse selection.
- **A trade prints at our price** without reaching us: a naive backtest counts that as our fill.
  In reality we were behind those shares.

Fill models that ignore queue position therefore overstate fills and, more importantly,
**mis-estimate which fills you get**. The fills a queue-aware model gives you are skewed toward
moments when the level is swept, which are exactly the moments the price moves against you.

## The three fill models
| model | a resting buy at P fills when... | bias |
|---|---|---|
| touch (naive) | any execution prints at P | far too many fills, too benign |
| queue (L3) | the shares ahead of us at P (measured on arrival) have executed or cancelled, and an execution reaches us; or an execution prints below P; or a sell arrives priced at or below P | realistic, given no impact |
| through (pessimistic) | an execution prints strictly below P (traded through), or a crossing sell arrives | too few fills |

With full L3 data the queue model is exact up to one assumption: **our order has no market
impact**. A real order at the back of the queue could change other participants' behaviour,
e.g. someone might cancel or join because the level grew. With only historical data that
counterfactual is unknowable. The assumption is standard and stated.

## Latency
Two independent delays:
- **Feed latency**: the strategy sees the book as it was `feed` µs ago.
- **Order latency**: an order joins the queue `order` µs after the decision.

Both mean the strategy acts on stale information and joins queues later, behind orders that
arrived in the meantime. The latency sweep shows how fill quality degrades from colocated (5 µs)
to retail-like (5 ms) latency.

## Markouts
A markout is the mid-price move after a fill, signed so that positive is in our favour:
`buy: mid(t+h) - fill price`, `sell: fill price - mid(t+h)`, per share.
- For a market maker, the spread capture is the markout at h → 0. Adverse selection is how much
  the markout decays as h grows.
- For a taker, the markout measures whether the signal predicted the move, and the fees and
  half-spread must be overcome.

## Avellaneda–Stoikov
The model (Avellaneda & Stoikov, 2008) solves a market maker's utility problem under Brownian mid
dynamics and exponentially decaying fill intensity `λ(δ) = A e^{-κδ}` at distance δ from the mid.
It gives a reservation price `r = s − q γ σ² τ`, skewed against inventory q, and a total spread
`γ σ² τ + (2/γ) ln(1 + γ/κ)`. queuesim uses the common intraday adaptation: a fixed short horizon τ
instead of time-to-close, inventory in lots, and σ from an EWMA of mid changes. The model assumes
fills arrive as a Poisson process independent of the price path. Real fills, as the queue model
shows, are **conditionally adverse**, which is why the model's theoretical profitability does not
survive realistic fill simulation without an adverse-selection term.

## Fees and rebates
Nasdaq-style maker/taker pricing: adding liquidity earns a rebate (here $0.0020/share), taking it
costs a fee (here $0.0030/share). For a passive strategy the rebate is often a large share of
gross PnL, so a backtest that gets the maker/taker split wrong gets the economics wrong.
