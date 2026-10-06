"""Regenerate every paper figure + stats from retained logic-analyzer offsets.
Input: <dataset>/timesync-2026-09-09/analysis/offsets.csv, <dataset>/sync-matrix/sync-matrix.csv
Run:   paperenv/.venv/bin/python make_figures.py
"""
import csv, json, math, statistics as st, sys
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Data root = the public dataset repo (github.com/IEatCodeDaily/ftm-camera-timesync-dataset).
# Works from <dataset>/paper/ or from the thesis repo's paper/ next to a local clone.
_here = Path(__file__).resolve().parent
DATA = next(d for d in (_here.parent, Path("/mnt/e/Projects/ftm-camera-timesync-dataset")) if (d / "timesync-2026-09-09").is_dir())
SRC = DATA / "timesync-2026-09-09/analysis/offsets.csv"
OUT = Path(__file__).parent / "figures"
OUT.mkdir(exist_ok=True)
rows = list(csv.DictReader(SRC.open()))

CAMPAIGN = "20260909-065110-campaign"           # common-firmware, counterbalanced block
CLEAN_FTM = ("20260908-173016/00-mcpwm-ftm", "20260908-173016/01-mcpwm-ftm", "20260908-174159/00-mcpwm-ftm")
BACKEND_RUN = "20260908-170156"                  # same build, backend swap


def sel(pred):
    return [r for r in rows if pred(r)]


def summ(v):
    a = sorted(abs(x) for x in v); n = len(v)
    q = lambda p: a[min(n - 1, round(p * (n - 1)))]   # nearest-sample quantile, index round(p(n-1)) (NumPy "nearest"), same rule as report
    return dict(n=n, mean=st.mean(v), sd=st.pstdev(v), rms=math.sqrt(sum(x * x for x in v) / n),
                p95=q(.95), p99=q(.99), max=a[-1])


src = {"No sync (local MAC)": "mcpwm:mac", "NTP-style UDP": "mcpwm:ntp", "AP TSF": "mcpwm:tsf",
       "FTM holdover": "mcpwm:frozen", "Live FTM": "mcpwm:ftm"}
stats = {}
data = []
for label, m in src.items():
    v = [float(r["offset_us"]) for r in sel(lambda r: CAMPAIGN in r["run"] and r["method"] == m)]
    stats[label] = summ(v); data.append([abs(x) for x in v])
hist = [r for r in rows if r["run"].endswith(CLEAN_FTM)]
stats["Live FTM, 3 repaired records"] = summ([float(r["offset_us"]) for r in hist])
load = [float(r["offset_us"]) for r in rows if r["method"] == "mcpwm:ftm-camera-load"]
stats["Live FTM, camera tracking load (provisional)"] = summ(load)
back = {"esp_timer callback": "esp:ftm", "FreeRTOS task": "task:ftm", "GPTimer (HW)": "gpt:ftm"}
for label, m in back.items():
    stats["Backend " + label] = summ([float(r["offset_us"]) for r in rows if BACKEND_RUN in r["run"] and r["method"] == m])

# self-check: headline numbers must match the published report (Table 4 / Table 1)
assert abs(stats["Live FTM"]["max"] - 1.688) < 1e-3, stats["Live FTM"]
assert abs(stats["NTP-style UDP"]["max"] - 55503.062) < 1e-2
assert abs(stats["Live FTM, 3 repaired records"]["max"] - 1.937) < 1e-3
assert stats["Live FTM"]["n"] == 180 and stats["Live FTM, 3 repaired records"]["n"] == 540

# --- Revision numbers (each quoted in main.tex) ---
import itertools
LIVE4 = (CAMPAIGN + "/00-00-mcpwm-ftm",) + CLEAN_FTM
def rec(run): return [r for r in rows if r["run"].endswith(run)]
# per-follower, per-record means (M7): range and sign pattern
fmeans = {(run, n): st.mean(float(r["offset_us"]) for r in rec(run) if r["node"] == n) for run in LIVE4 for n in "013"}
assert abs(min(fmeans.values()) + 0.523) < 1e-3 and abs(max(fmeans.values()) - 0.302) < 1e-3, fmeans
assert all(fmeans[(LIVE4[0], n)] > 0 for n in "013")                         # C1: all positive
assert sum(fmeans[(r, n)] < 0 for r in CLEAN_FTM for n in "013") == 8        # H1-H3: 8 of 9 negative
# three-cornered hat on simultaneous offsets d_i = f_i - ref: cov(d_i,d_j) = var(ref)
def tch(run):
    by = {}
    for r in rec(run): by.setdefault(round(float(r["time_s"])), {})[r["node"]] = float(r["offset_us"])
    full = [v for v in by.values() if len(v) == 3]
    d = {n: [v[n] for v in full] for n in "013"}
    cov = lambda a, b: st.mean((x - st.mean(a)) * (y - st.mean(b)) for x, y in zip(a, b))
    pairs = list(itertools.combinations("013", 2))
    vref = st.mean(cov(d[a], d[b]) for a, b in pairs)
    return dict(ref=math.sqrt(vref), fol=[math.sqrt(st.pvariance(d[n]) - vref) for n in "013"],
                ff=[st.pstdev([x - y for x, y in zip(d[a], d[b])]) for a, b in pairs],
                corr=[cov(d[a], d[b]) / (st.pstdev(d[a]) * st.pstdev(d[b])) for a, b in pairs],
                share=[vref / st.pvariance(d[n]) for n in "013"])
TCH = {run: tch(run) for run in LIVE4}
ref_sd = [t["ref"] for t in TCH.values()]; fol_sd = [x for t in TCH.values() for x in t["fol"]]
ff_sd = [x for t in TCH.values() for x in t["ff"]]; corr = [x for t in TCH.values() for x in t["corr"]]
share = [x for t in TCH.values() for x in t["share"]]
assert (round(min(ref_sd), 2), round(max(ref_sd), 2)) == (0.36, 0.46), ref_sd
assert (round(min(fol_sd), 2), round(max(fol_sd), 2)) == (0.32, 0.59), fol_sd
assert (round(min(ff_sd), 2), round(max(ff_sd), 2)) == (0.53, 0.78), ff_sd
assert (round(min(corr), 2), round(max(corr), 2)) == (0.34, 0.59), corr
assert (round(min(share), 1), round(max(share), 1)) == (0.3, 0.6), share
# MCPWM tick 1 us (resolution_hz=1000000, archived strobe_gpio.c) -> uniform quantization SD
assert round(1 / math.sqrt(12), 2) == 0.29
# excluded 25-s MCPWM live-FTM records (short-record sensitivity, M4)
short = [abs(float(r["offset_us"])) for r in rows if "20260908-171055" in r["run"] and r["method"] == "mcpwm:ftm"]
assert len(short) == 150 and abs(max(short) - 2.375) < 1e-3
# no-sync drift: per-follower linear trend over each campaign record (us/s)
def slope(pts):
    t = [p[0] for p in pts]; y = [p[1] for p in pts]; mt, my = st.mean(t), st.mean(y)
    return sum((a - mt) * (b - my) for a, b in zip(t, y)) / sum((a - mt) ** 2 for a in t)
drift = [abs(slope([(float(r["time_s"]), float(r["offset_us"])) for r in rows if r["run"] == run and r["node"] == n]))
         for run in {r["run"] for r in rows if CAMPAIGN in r["run"] and r["method"] == "mcpwm:mac"} for n in "013"]
assert (round(min(drift), 1), round(max(drift), 1)) == (0.4, 6.1), drift
# improvement factors (M5): worst case TSF / live FTM ~ 10^2; NTP-style / live FTM > 10^4
assert 100 < 182.188 / stats["Live FTM"]["max"] < 120 and 55503.062 / stats["Live FTM"]["max"] > 1e4
# 4 live-FTM records pooled: SD 0.66, max 1.937
pool4 = summ([float(r["offset_us"]) for run in LIVE4 for r in rec(run)])
assert pool4["n"] == 720 and round(pool4["sd"], 2) == 0.66 and abs(pool4["max"] - 1.937) < 1e-3

# Per-record table (three followers pooled per record), emitted as LaTeX so no number is hand-copied
def fmt(x): return f"{x:.2f}" if x < 100 else f"{x:,.0f}".replace(",", "\\,")
rec_rows = []
for label, m in src.items():
    for run in sorted({r["run"] for r in rows if CAMPAIGN in r["run"] and r["method"] == m}):
        rec_rows.append((label, f"C{int(run.split('/')[-1][:2]) + 1}", [float(r["offset_us"]) for r in rows if r["run"] == run]))
for i, run in enumerate(CLEAN_FTM):
    rec_rows.append(("Live FTM", f"H{i + 1}", [float(r["offset_us"]) for r in rows if r["run"].endswith(run)]))
rec_rows.append(("\\textit{Camera load}", "\\textit{L}", load))   # provisional row set in italics
tex = []
for label, rec, v in rec_rows:
    s = summ(v)
    tex.append(f"{label} & {rec} & {s['n']} & {('+' if s['mean'] >= 0 else '$-$') + fmt(abs(s['mean']))} & {fmt(s['sd'])} & {fmt(s['p95'])} & {fmt(s['max'])} \\\\")
out, prev = [], None
for l in tex:                                   # thin gap between sources
    k = l.split(" & ")[0].replace("\\textit{Camera load}", "Live FTM")
    if prev and k != prev: out.append("\\addlinespace[1pt]")
    out.append(l); prev = k
(OUT / "table_records.tex").write_text("\n".join(out) + "\n")
assert sum(1 for l, *_ in rec_rows if l == "Live FTM") == 4 and len(rec_rows) == 13, len(rec_rows)

plt.rcParams.update({"font.size": 8, "font.family": "serif", "font.serif": ["STIXGeneral", "DejaVu Serif"], "mathtext.fontset": "stix", "pdf.fonttype": 42, "axes.grid": True, "grid.alpha": .3})

# Fig 2: source comparison, |offset| log scale
fig, ax = plt.subplots(figsize=(3.5, 2.4))
ax.boxplot(data, whis=(0, 100), widths=.55, medianprops=dict(color="k"))
ax.set_yscale("log"); ax.set_ylabel(r"|follower $-$ reference| ($\mu$s)")
ax.set_xticks(range(1, 6), ["No\nsync", "NTP\nstyle", "AP\nTSF", "FTM\nhold", "Live\nFTM"])
ax.axhline(1000, ls="--", lw=.8, color="0.4"); ax.text(4.6, 1250, "1 ms", ha="center", fontsize=8, color="0.3")
fig.tight_layout(); fig.savefig(OUT / "source_comparison.pdf"); plt.close(fig)

# Fig 3: live-FTM offset time series, 3 repaired records + campaign record
fig, ax = plt.subplots(figsize=(3.5, 2.2))
mk = {"0": "o", "1": "s", "3": "^"}
for node in "013":
    pts = [r for r in hist + sel(lambda r: CAMPAIGN in r["run"] and r["method"] == "mcpwm:ftm") if r["node"] == node]
    # concatenate records on one axis: record index * 65 s + t
    runs = sorted({r["run"] for r in pts})
    xs = [runs.index(r["run"]) * 65 + float(r["time_s"]) for r in pts]
    ax.plot(xs, [float(r["offset_us"]) for r in pts], mk[node], ms=2, mfc="none", label=f"node {node}")
for k in range(1, 4): ax.axvline(k * 65 - 2.5, color="0.6", lw=.6)
ax.axhline(0, color="k", lw=.6)
ax.set_xlabel("time, four 60-s records concatenated (s)"); ax.set_ylabel(r"offset ($\mu$s)")
ax.legend(ncol=3, fontsize=7, loc="lower center", bbox_to_anchor=(.5, 1.0), frameon=False); ax.set_ylim(-2.2, 2.2)
fig.tight_layout(); fig.savefig(OUT / "ftm_timeseries.pdf"); plt.close(fig)


# Fig 1: system + measurement schematic (vector, no data). Bus layout: no crossing lines; all text 8 pt.
fig, ax = plt.subplots(figsize=(3.5, 2.3)); ax.set_axis_off(); ax.set_xlim(0, 10); ax.set_ylim(0, 6.6)
def box(x, y, w, h, t, fc="white"):
    ax.add_patch(plt.Rectangle((x, y), w, h, fc=fc, ec="k", lw=.7))
    ax.text(x + w / 2, y + h / 2, t, ha="center", va="center", fontsize=8, linespacing=1.1)
def arrow(a, b, **k):
    ax.annotate("", b, a, arrowprops=dict(arrowstyle="->", lw=.7, shrinkA=0, shrinkB=0, **k))
box(2.6, 5.6, 4.8, .9, "host: hotspot AP, triangulation", "0.93")
xs = [.15, 2.65, 5.15, 7.65]; w = 2.2
ax.plot([.5, 9.5], [4.6, 4.6], ls="--", lw=.9, color="#1f77b4")               # shared Wi-Fi channel
ax.text(.5, 4.75, "Wi-Fi: centroids (UDP), FTM to node 2", ha="left", va="bottom", fontsize=8, color="#1f77b4")
arrow((7.0, 4.6), (7.0, 5.6), color="0.4")
for i, x in enumerate(xs):
    ref = i == 2
    box(x, 2.5, w, 1.5, f"node {i}\n" + ("FTM\nresponder" if ref else "FTM\ninitiator"), "#dbe9f6" if ref else "white")
    ax.plot([x + w / 2] * 2, [4.0, 4.6], color="#1f77b4", lw=.9)               # radio stub
    arrow((x + w / 2, 2.5), (x + w / 2, 1.2), color="k")                      # GPIO41 to analyzer
ax.text(1.4, 1.85, "GPIO41", fontsize=8, ha="left", va="center")
box(.15, .2, 9.7, 1.0, "logic analyzer, 16 MS/s", "0.93")
fig.tight_layout(pad=.1); fig.savefig(OUT / "system.pdf"); plt.close(fig)


sync = list(csv.DictReader((DATA / "sync-matrix/sync-matrix.csv").open()))
SM = {(r["resolution"], r["fps"]): r for r in sync}   # triggered-capture matrix quoted in Sec. IV-E
assert [(SM[k]["median_us"], SM[k]["p95_us"], SM[k]["yield_pct"]) for k in
        [("qvga", "10"), ("qvga", "15"), ("qvga", "30"), ("qvga", "60"), ("vga", "10"), ("vga", "15"), ("vga", "30")]] == [
    ("28", "166", "100.0"), ("43", "171", "52.0"), ("32", "303", "59.0"), ("30", "285", "46.0"),
    ("107", "1276", "32.0"), ("", "", "0.0"), ("596", "1520", "62.0")], SM
json.dump({"offset_stats": stats, "sync_matrix_3cam": sync, "follower_means": {f"{k[0]}|{k[1]}": v for k, v in fmeans.items()},
           "three_cornered_hat": TCH, "nosync_drift_us_per_s": drift, "short_mcpwm_max": max(short)}, (OUT / "stats.json").open("w"), indent=1)
for k, s in stats.items():
    print(f"{k:45s} n={s['n']:4d} mean={s['mean']:+11.3f} sd={s['sd']:10.3f} p95={s['p95']:10.3f} p99={s['p99']:10.3f} max={s['max']:10.3f}")
print("figures ->", OUT)
