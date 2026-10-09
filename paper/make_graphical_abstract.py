"""Graphical abstract for the IEEE Sensors Letters submission: two panels (clock layer, capture layer).
Numbers are read from the dataset files and asserted, never typed.
Output: figures/graphical_abstract.{png,pdf} (1200x600 px at 200 dpi).
"""
import csv, json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

_here = Path(__file__).resolve().parent
DATA = next(d for d in (_here.parent, Path("/mnt/e/Projects/ftm-camera-timesync-dataset")) if (d / "timesync-2026-09-09").is_dir())
rows = list(csv.DictReader((DATA / "timesync-2026-09-09/analysis/offsets.csv").open()))
CAMPAIGN = "20260909-065110-campaign"
src = {"NTP-style UDP\n(unfiltered)": "mcpwm:ntp", "AP TSF via\nESP-IDF API": "mcpwm:tsf", "Live FTM": "mcpwm:ftm"}   # rev10 M2: TSF as exposed by the API
worst = {k: max(abs(float(r["offset_us"])) for r in rows if CAMPAIGN in r["run"] and r["method"] == m) for k, m in src.items()}
assert [round(v, 1) for v in worst.values()] == [55503.1, 182.2, 1.7], worst

R4 = next(r for r in json.load((DATA / "tracking/stats.json").open())["recordings"] if r["file"].startswith("tracking-1791273844728"))
AB = json.load((DATA / "tracking/ab/tracking_ab.stats.json").open())   # same-session A/B, 4 cameras VGA 35 frames/s, 600 s each
row = R4["C_marker_row_time_us"]["median"]
cap = {"software path, delay-\nremoved spread, med.": AB["sw"]["jitter_fixed_removed_us"]["median"],
       "software path, p95": AB["sw"]["jitter_fixed_removed_us"]["p95"],
       "hardware timing, delay-\nremoved spread, med.": AB["hw"]["jitter_fixed_removed_us"]["median"],
       "hardware timing, p95": AB["hw"]["jitter_fixed_removed_us"]["p95"],
       "row-readout residual after\nhost correction, med. (up to)": row / 2}
assert [round(v, 1) for v in cap.values()] == [40.7, 236.0, 2.1, 4.1, 2492.9], cap   # rev10 M6: residual = half the corrected row offset

plt.rcParams.update({"font.family": "serif", "font.serif": ["STIXGeneral", "DejaVu Serif"], "mathtext.fontset": "stix", "pdf.fonttype": 42})
fig = plt.figure(figsize=(6, 3), dpi=200)
fig.text(.5, .955, "Wi-Fi FTM clock alignment for wireless ESP32-S3 + OV5640 infrared tracking", ha="center", fontsize=10, weight="bold")


def panel(x0, title, note, items, hi, color):
    fig.text(x0 + .22, .86, title, ha="center", fontsize=9, weight="bold", color=color)
    ax = fig.add_axes([x0 + .14, .27, .3, .5])
    names, vals = list(items), list(items.values())
    ax.hlines(range(len(vals)), .5, vals, color="0.8", lw=1)   # dots, not bars: bar length on a log axis misleads
    ax.scatter(vals, range(len(vals)), s=[60 if i == hi else 30 for i in range(len(vals))],
               color=[color if i == hi else "0.5" for i in range(len(vals))], zorder=3)
    ax.set_xscale("log"); ax.set_xlim(.5, 3e6); ax.set_yticks(range(len(vals)), names, fontsize=7)
    ax.tick_params(axis="x", labelsize=6.5); ax.spines[["top", "right"]].set_visible(False)
    for y, v in enumerate(vals):
        lab = f"{v / 1000:,.1f} ms" if v >= 1000 else (f"{v:.0f} µs" if v > 10 or v == int(v) else f"{v:.1f} µs")
        ax.text(v * 1.9, y, lab, va="center", fontsize=8, weight="bold" if y == hi else "normal")
    ax.set_xlabel("µs (log scale)", fontsize=7)
    fig.text(x0 + .22, .06, note, ha="center", va="center", fontsize=6.8, style="italic")


panel(0, "1  Clock layer (GPIO, logic analyzer)",
      "largest observed follower-to-reference GPIO offset\n(campaign 60-s records), cameras idle; separate build",
      worst, 2, "#1f77b4")
panel(.5, "2  Capture layer (4-camera tracking)",
      "in each node's own clock estimate; trigger + stamp switched together;\nframe-start edges, not exposure; one paired 600-s run, VGA 35 frames/s",
      cap, 2, "#d62728")
out = Path(__file__).parent / "figures"
fig.savefig(out / "graphical_abstract.png"); fig.savefig(out / "graphical_abstract.pdf")
print("graphical abstract ->", out)
