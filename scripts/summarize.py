#!/usr/bin/env python3
import glob, json, os, sys
d = sys.argv[1]
R = {os.path.basename(f)[:-5]: json.load(open(f)) for f in sorted(glob.glob(os.path.join(d, "*.json")))}
print(f"# queuesim experiments ({os.path.basename(d)})\n")
print("## 1. Fill models: Avellaneda-Stoikov market maker (100-share quotes, 50 us feed/order latency)\n")
print("| symbol | model | fills | maker volume | PnL $ | PnL ex fees $ | rebates/fees $ | max |pos| | maker markout 1s / 5s / 30s (c/share) |")
print("|---|---|---|---|---|---|---|---|---|")
for k, r in R.items():
    if not k.startswith("as_") or "lat" in k or "rerun" in k: continue
    for x in r["results"]:
        m = x["markout_maker_per_share"]
        print(f'| {x["symbol"]} | {x["model"]} | {x["fills"]:,} | {x["maker_volume"]:,} | {x["pnl"]:,.0f} | {x["pnl_ex_fees"]:,.0f} | {-x["fees"]:,.0f} | {x["max_abs_position"]:,} | {100*m["1s"]:.2f} / {100*m["5s"]:.2f} / {100*m["30s"]:.2f} |')
print("\n## 2. Latency sensitivity (MSFT, queue model)\n")
print("| feed = order latency | fills | maker volume | PnL $ | maker markout 1s (c/share) |")
print("|---|---|---|---|---|")
lat = sorted((k for k in R if "_lat" in k), key=lambda s: int(s.split("lat")[1][:-2]))
for k in lat:
    x = R[k]["results"][0]
    print(f'| {R[k]["feed_latency_us"]:.0f} us | {x["fills"]:,} | {x["maker_volume"]:,} | {x["pnl"]:,.0f} | {100*x["markout_maker_per_share"]["1s"]:.2f} |')
print("\n## 3. Imbalance taker (queue model, 100 shares, 5 s hold)\n")
print("| symbol | theta | round trips (fills/2) | PnL $ | PnL ex fees $ | taker markout 1s / 5s / 30s (c/share) |")
print("|---|---|---|---|---|---|")
for k, r in R.items():
    if not k.startswith("imb_"): continue
    x = r["results"][0]; m = x["markout_taker_per_share"]
    print(f'| {x["symbol"]} | {r["params"]["theta"]} | {x["fills"]//2:,} | {x["pnl"]:,.0f} | {x["pnl_ex_fees"]:,.0f} | {100*m["1s"]:.2f} / {100*m["5s"]:.2f} / {100*m["30s"]:.2f} |')
if "as_AAPL" in R and "as_AAPL_rerun" in R:
    same = [a["fill_hash"] == b["fill_hash"] and a["pnl"] == b["pnl"] for a, b in zip(R["as_AAPL"]["results"], R["as_AAPL_rerun"]["results"])]
    print(f"\n## 4. Determinism\n\nAAPL rerun identical fill hashes and PnL for all models: {all(same)}")
la = sum(x["lookahead_violations"] for r in R.values() for x in r["results"])
print(f"\nLook-ahead violations across all runs: {la}")
secs = [r["seconds"] for r in R.values()]
print(f"Wall time per full-day run (decompress + replay + all models): {min(secs):.1f}-{max(secs):.1f} s")
