# queuesim experiments (20261102-020741)

## 1. Fill models: Avellaneda-Stoikov market maker (100-share quotes, 50 us feed/order latency)

| symbol | model | fills | maker volume | PnL $ | PnL ex fees $ | rebates/fees $ | max |pos| | maker markout 1s / 5s / 30s (c/share) |
|---|---|---|---|---|---|---|---|---|
| AAPL | queue | 13,871 | 994,361 | -7,746 | -9,665 | 1,919 | 672 | -0.65 / -0.81 / -1.05 |
| AAPL | touch | 20,222 | 1,306,781 | -8,419 | -10,934 | 2,515 | 667 | -0.45 / -0.58 / -0.81 |
| AAPL | through | 10,338 | 818,118 | -7,713 | -9,293 | 1,580 | 683 | -0.86 / -0.97 / -1.19 |
| CSCO | queue | 256 | 19,211 | 87 | 49 | 38 | 615 | -0.63 / -0.68 / -0.63 |
| CSCO | touch | 350 | 26,169 | 93 | 41 | 52 | 654 | -0.39 / -0.49 / -0.60 |
| CSCO | through | 163 | 15,163 | -87 | -117 | 30 | 537 | -0.97 / -0.69 / -1.12 |
| INTC | queue | 274 | 24,391 | 139 | 90 | 49 | 590 | -0.33 / -0.14 / -0.71 |
| INTC | touch | 422 | 35,298 | 41 | -29 | 70 | 593 | -0.10 / -0.01 / -0.47 |
| INTC | through | 202 | 19,029 | 9 | -30 | 38 | 590 | -0.63 / -0.63 / -1.36 |
| MSFT | queue | 1,880 | 150,933 | 603 | 304 | 299 | 593 | -0.55 / -0.61 / -0.97 |
| MSFT | touch | 2,699 | 198,740 | 422 | 28 | 394 | 599 | -0.33 / -0.38 / -0.68 |
| MSFT | through | 1,304 | 118,295 | 328 | 95 | 233 | 589 | -0.79 / -0.85 / -1.11 |
| QQQ | queue | 2,866 | 241,915 | -970 | -1,449 | 480 | 673 | -0.53 / -0.52 / -0.58 |
| QQQ | touch | 3,567 | 275,240 | -420 | -968 | 547 | 694 | -0.40 / -0.34 / -0.43 |
| QQQ | through | 2,165 | 203,471 | -1,353 | -1,757 | 404 | 689 | -0.76 / -0.76 / -0.85 |

## 2. Latency sensitivity (MSFT, queue model)

| feed = order latency | fills | maker volume | PnL $ | maker markout 1s (c/share) |
|---|---|---|---|---|
| 5 us | 1,886 | 151,150 | 651 | -0.54 |
| 50 us | 1,880 | 150,933 | 603 | -0.55 |
| 500 us | 1,996 | 159,313 | 670 | -0.59 |
| 5000 us | 2,077 | 162,939 | 488 | -0.60 |

## 3. Imbalance taker (queue model, 100 shares, 5 s hold)

| symbol | theta | round trips (fills/2) | PnL $ | PnL ex fees $ | taker markout 1s / 5s / 30s (c/share) |
|---|---|---|---|---|---|
| AAPL | 0.6 | 3,937 | -9,503 | -7,510 | -1.15 / -1.19 / -1.11 |
| AAPL | 0.8 | 3,914 | -8,977 | -7,130 | -1.21 / -1.15 / -1.10 |
| AAPL | 0.9 | 3,827 | -8,864 | -7,158 | -1.27 / -1.20 / -1.20 |
| MSFT | 0.6 | 3,133 | -4,162 | -2,386 | -0.45 / -0.46 / -0.42 |
| MSFT | 0.8 | 2,867 | -3,767 | -2,225 | -0.51 / -0.50 / -0.47 |
| MSFT | 0.9 | 2,530 | -3,358 | -2,080 | -0.56 / -0.57 / -0.58 |

## 4. Determinism

AAPL rerun identical fill hashes and PnL for all models: True

Look-ahead violations across all runs: 0
Wall time per full-day run (decompress + replay + all models): 14.7-28.0 s
