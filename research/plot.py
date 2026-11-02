#!/usr/bin/env python3
"""Plots for the README from a results directory: fill-model comparison and markout curves."""
import glob, json, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
d, out = sys.argv[1], sys.argv[2]
syms = ["AAPL", "MSFT", "QQQ", "INTC", "CSCO"]
R = {s: json.load(open(os.path.join(d, f"as_{s}.json")))["results"] for s in syms}
models = ["touch", "queue", "through"]
fig, ax = plt.subplots(1, 2, figsize=(11, 4.2))
w = 0.26
for i, m in enumerate(models):
    rel = [next(x for x in R[s] if x["model"] == m)["maker_volume"] / next(x for x in R[s] if x["model"] == "queue")["maker_volume"] for s in syms]
    ax[0].bar([k + (i - 1) * w for k in range(len(syms))], rel, w, label=m)
ax[0].set_xticks(range(len(syms)), syms)
ax[0].axhline(1, color="k", lw=0.6)
ax[0].set_ylabel("maker volume relative to queue model")
ax[0].set_title("Fills by fill model (Avellaneda-Stoikov)")
ax[0].legend()
hs = [1, 5, 30]
for m, ls in zip(models, ["--", "-", ":"]):
    ys = [sum(next(x for x in R[s] if x["model"] == m)["markout_maker_per_share"][f"{h}s"] for s in syms) / len(syms) * 100 for h in hs]
    ax[1].plot(hs, ys, ls, marker="o", label=m)
ax[1].set_xscale("log")
ax[1].set_xticks(hs, [f"{h}s" for h in hs])
ax[1].axhline(0, color="k", lw=0.6)
ax[1].set_ylabel("maker markout (cents/share, mean of 5 symbols)")
ax[1].set_title("Adverse selection is understated by naive fills")
ax[1].legend()
fig.tight_layout()
fig.savefig(out, dpi=130)
print("wrote", out)
