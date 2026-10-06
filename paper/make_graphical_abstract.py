"""Graphical abstract for the IEEE Sensors Letters submission.
Reuses make_figures.py's data source; numbers are recomputed and asserted, not typed.
Output: figures/graphical_abstract.{png,pdf} (1200x600 px at 200 dpi).
"""
import csv
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

_here = Path(__file__).resolve().parent
DATA = next(d for d in (_here.parent, Path("/mnt/e/Projects/ftm-camera-timesync-dataset")) if (d / "timesync-2026-09-09").is_dir())
rows = list(csv.DictReader((DATA / "timesync-2026-09-09/analysis/offsets.csv").open()))
CAMPAIGN = "20260909-065110-campaign"
src = {"NTP-style\nUDP": "mcpwm:ntp", "AP beacon\n(TSF)": "mcpwm:tsf", "Live FTM\n(this work)": "mcpwm:ftm"}
worst = {k: max(abs(float(r["offset_us"])) for r in rows if CAMPAIGN in r["run"] and r["method"] == m) for k, m in src.items()}
assert [round(v, 1) for v in worst.values()] == [55503.1, 182.2, 1.7], worst

plt.rcParams.update({"font.family": "serif", "font.serif": ["STIXGeneral", "DejaVu Serif"], "mathtext.fontset": "stix", "pdf.fonttype": 42})
fig = plt.figure(figsize=(6, 3), dpi=200)

# Left: what was built and how it was measured
ax = fig.add_axes([0, 0, .5, 1]); ax.set_axis_off(); ax.set_xlim(0, 10); ax.set_ylim(0, 10)
ax.text(5, 9.3, "Wire-free clock alignment of\nESP32-S3 + OV5640 IR camera nodes", ha="center", va="top", fontsize=10.5, weight="bold")
for i, x in enumerate([.6, 3.0, 5.4, 7.8]):
    ref = i == 2
    ax.add_patch(plt.Rectangle((x, 4.2), 1.8, 1.6, fc="#dbe9f6" if ref else "white", ec="k", lw=.8))
    ax.text(x + .9, 5.0, f"node {i}\n" + ("FTM\nresponder" if ref else "FTM\ninitiator"), ha="center", va="center", fontsize=7.5)
    ax.plot([x + .9] * 2, [5.8, 6.6], color="#1f77b4", lw=1)
    ax.annotate("", (x + .9, 2.6), (x + .9, 4.2), arrowprops=dict(arrowstyle="->", lw=.8))
ax.plot([.8, 9.4], [6.6, 6.6], ls="--", color="#1f77b4", lw=1)
ax.text(5, 6.85, "Wi-Fi Fine Timing Measurement (802.11 FTM)", ha="center", fontsize=8, color="#1f77b4")
ax.add_patch(plt.Rectangle((.6, 1.4), 9.0, 1.2, fc="0.93", ec="k", lw=.8))
ax.text(5.1, 2.0, "logic analyzer: 1 pulse/s per node, 16 MS/s", ha="center", va="center", fontsize=8)
ax.text(5, .55, "same firmware and output path for every clock source", ha="center", fontsize=7.5, style="italic")

# Right: the result, worst follower-to-reference offset, log scale
ax = fig.add_axes([.62, .2, .35, .62])
names, vals = list(worst), list(worst.values())
# Dots, not bars: bar length on a log axis would misstate the ratios.
ax.hlines(range(3), .5, vals, color="0.8", lw=1)
ax.scatter(vals, range(3), s=[30, 30, 60], color=["0.55", "0.4", "#1f77b4"], zorder=3)
ax.set_xscale("log"); ax.set_xlim(.5, 2e6); ax.set_yticks(range(3), names, fontsize=8)
ax.set_xlabel("worst offset between nodes, µs (log scale)\n60-s records, 3 followers, 16 MS/s", fontsize=7.5); ax.tick_params(axis="x", labelsize=7)
for y, v in enumerate(vals):
    lab = f"{v:,.0f} µs" if v > 10 else f"{v:.1f} µs"
    ax.text(v * 1.8, y, lab, va="center", fontsize=8.5, weight="bold" if v < 10 else "normal")
ax.spines[["top", "right"]].set_visible(False)
fig.text(.795, .9, "~100× better than beacon timing", ha="center", fontsize=9.5, weight="bold", color="#1f77b4")

out = Path(__file__).parent / "figures"
fig.savefig(out / "graphical_abstract.png"); fig.savefig(out / "graphical_abstract.pdf")
print("graphical abstract ->", out)
