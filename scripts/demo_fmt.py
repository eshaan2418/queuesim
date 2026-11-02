import json, sys
d = json.load(sys.stdin)
print(f'{d["results"][0]["symbol"]}: replayed {d["itch_messages"]:,} ITCH messages ({d["symbol_messages"]:,} for the symbol) in {d["seconds"]:.1f} s')
print(f'{"model":8} {"fills":>8} {"maker vol":>10} {"PnL $":>10} {"markout 1s c/sh":>16}')
for x in d["results"]:
    print(f'{x["model"]:8} {x["fills"]:>8,} {x["maker_volume"]:>10,} {x["pnl"]:>10,.0f} {100*x["markout_maker_per_share"]["1s"]:>16.2f}')
