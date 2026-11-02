#!/usr/bin/env bash
# Full-day experiments on the NASDAQ 2019-01-30 ITCH sample. Raw JSON in research/results/<stamp>/.
set -euo pipefail
ITCH="${ITCH:-$HOME/quantfactory/_data/itch/01302019.NASDAQ_ITCH50.gz}"
B="${BUILD:-build}"
OUT="research/results/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"
{ date -u +%FT%TZ; git rev-parse --short HEAD 2>/dev/null || true; (sysctl -n machdep.cpu.brand_string 2>/dev/null || true); uptime; } > "$OUT/machine.txt"
run() { local name="$1"; shift; echo "[$name]" >&2; "$B/queuesim" --itch "$ITCH" --out "$OUT/$name.json" "$@"; }
# 1. Fill-model comparison, Avellaneda-Stoikov, five liquid names.
for s in AAPL MSFT INTC CSCO QQQ; do run "as_${s}" --symbol "$s" --strategy as --model all; done
# 2. Latency sensitivity (queue model), MSFT.
for l in 5 50 500 5000; do run "as_MSFT_lat${l}us" --symbol MSFT --strategy as --model queue --feed-us "$l" --order-us "$l"; done
# 3. Order-book imbalance taker.
for th in 0.6 0.8 0.9; do run "imb_AAPL_theta${th}" --symbol AAPL --strategy imb --model queue --theta "$th"; done
for th in 0.6 0.8 0.9; do run "imb_MSFT_theta${th}" --symbol MSFT --strategy imb --model queue --theta "$th"; done
# 4. Determinism: identical rerun.
run "as_AAPL_rerun" --symbol AAPL --strategy as --model all
python3 scripts/summarize.py "$OUT" | tee "$OUT/SUMMARY.md"
