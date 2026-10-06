"""Regenerate every paper figure + stats from retained logic-analyzer offsets.
Input: <dataset>/timesync-2026-09-09/analysis/offsets.csv, <dataset>/tracking/stats.json, <dataset>/wrap-run/wrap-40min.jsonl.stats.json
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
assert round(stats["AP TSF"]["max"] / stats["Live FTM"]["max"]) == 108                     # "108x" in text
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
fig, ax = plt.subplots(figsize=(3.5, 1.55))
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


# ---- Clock layer: timer-backend pair on build 8dc05893 (TRACKING_RESULTS.md section F) ----
PAIR = "20260908-171055"
def worst(m):   # worst per-node population SD and worst |offset| over both 25-s repeats
    rr = [r for r in rows if PAIR in r["run"] and r["method"] == m]
    sds = [st.pstdev([float(r["offset_us"]) for r in rr if r["run"] == run and r["node"] == n])
           for run in {r["run"] for r in rr} for n in "013"]
    return max(sds), max(abs(float(r["offset_us"])) for r in rr), len({r["run"] for r in rr})
mc, gp = worst("mcpwm:ftm"), worst("gptpair:ftm")
assert (round(mc[0], 3), round(mc[1], 3), mc[2]) == (0.714, 2.375, 2), mc
assert (round(gp[0], 3), round(gp[1], 3), gp[2]) == (0.977, 2.124, 2), gp
assert mc[0] < gp[0] and gp[1] < mc[1]          # neither wins both metrics -> not ranked in the text
bk = {k: stats["Backend " + k] for k in back}
assert (round(bk["esp_timer callback"]["sd"], 1), round(bk["FreeRTOS task"]["sd"], 2), round(bk["GPTimer (HW)"]["sd"], 2)) == (25.7, 1.08, 0.52), bk

# ---- Capture layer: tracking recordings + 40-min FREX run (dataset tracking/, wrap-run/) ----
TS = json.load((DATA / "tracking/stats.json").open())
REC = {r["file"][9:22]: r for r in TS["recordings"]}
R4, R15b, R04, R09, R11 = (REC[k] for k in ("1791273844728", "1791274143423", "1791266693622", "1791266988742", "1791267118434"))
assert R4["cameras"] == [0, 1, 2, 3] and R4["inferred_fps"] == 35 and R4["resolution"] == [640, 480] and R4["row_period_ns"] == 25750
WR = json.load((DATA / "wrap-run/wrap-40min.jsonl.stats.json").open())
dly = R4["B_trigger_to_capture_us_median"].values(); wdl = WR["per_node_median_delay_us"].values()
jit, raw = R4["B_capture_spread_after_fixed_offset_us"], R4["B_capture_spread_raw_us"]
row, xrow = R4["C_marker_row_time_us"], R4["C_cross_camera_row_time_diff_us"]
det = [c["detection_us_median"] for r in (R4, R15b) for c in r["per_camera"].values()]
d2 = [R4["D_static_2d"][c][k] for c in "23" for k in ("sd_x_px_median", "sd_y_px_median")]
L9, L11 = R09["E_3d"]["L_calibrator"], R11["E_3d"]["L_calibrator"]
rep = {g["recording"][9:22]: g for g in TS["G_replay"]}["1791273844728"]
LIVE = stats["Live FTM"]["max"]
N = {  # every capture-layer number printed in main.tex / table_capture.tex
    "TrkDur": f"{R4['duration_s']:.0f}", "TrkFps": f"{R4['inferred_fps']}", "RowUs": f"{R4['row_period_ns'] / 1000:.2f}",
    "RowSpanMs": f"{R4['C_full_frame_row_span_us'] / 1000:.2f}",
    "TrkSlots": f"{100 * R4['frac_slots_all_paced_cams']:.1f}", "TrkSlotsN": f"{R4['slots_all_paced_cams']:,}".replace(",", "{,}"),
    "TrkSlotsAll": f"{R4['slots_spanned']:,}".replace(",", "{,}"),
    "DelayLo": f"{min(dly):.0f}", "DelayHi": f"{max(dly):.0f}",
    "JitMed": f"{jit['median']:.0f}", "JitPn": f"{jit['p95']:.0f}", "RawMed": f"{raw['median']:.0f}", "RawPn": f"{raw['p95']:.0f}",
    "RowMedMs": f"{row['median'] / 1000:.1f}", "RowPnMs": f"{row['p95'] / 1000:.1f}",
    "XrowMed": f"{xrow['median']:.0f}", "XrowPn": f"{xrow['p95']:.0f}",
    "XrowOverJit": f"{xrow['median'] / jit['median']:.0f}", "XrowOverClk": f"{xrow['median'] / mc[1]:.0f}",
    "DetLo": f"{min(det) / 1000:.2f}", "DetHi": f"{max(det) / 1000:.2f}",
    "CentLo": f"{min(d2):.3f}", "CentHi": f"{max(d2):.3f}", "CentWin": f"{R4['D_static_2d']['2']['windows']}",
    "SDthreeD": f"{R04['E_3d']['static_3d']['sd_3d_mm_median']:.2f}", "SDthreeDwin": f"{R04['E_3d']['static_3d']['windows']}",
    "ResFour": f"{R04['E_3d']['reproj_residual_px_max_of_2_views']['median']:.2f}",
    "LnineA": f"{L9['median_mm'][0]:.2f}", "LnineB": f"{L9['median_mm'][1]:.2f}", "LnineC": f"{L9['median_mm'][2]:.2f}",
    "LnineSD": f"{min(L9['sd_mm']):.2f}--{max(L9['sd_mm']):.2f}", "LnineN": f"{L9['frames']}", "LnineRes": f"{L9['reproj_residual_px']['median']:.2f}",
    "LelevA": f"{L11['median_mm'][0]:.2f}", "LelevB": f"{L11['median_mm'][1]:.2f}", "LelevC": f"{L11['median_mm'][2]:.2f}",
    "LelevSD": f"{min(L11['sd_mm']):.1f}--{max(L11['sd_mm']):.1f}", "LelevIQR": f"{min(L11['iqr_mm']):.1f}--{max(L11['iqr_mm']):.1f}",
    "LelevN": f"{L11['frames']:,}".replace(",", "{,}"), "LelevRes": f"{L11['reproj_residual_px']['median']:.2f}",
    "Quorum": f"{rep['quorum_yield']:.3f}",
    "OldDelayLo": f"{min(R04['B_trigger_to_capture_us_median'].values()) / 1000:.1f}",
    "OldDelayHi": f"{max(v for r in (R09, R11) for v in r['B_trigger_to_capture_us_median'].values()) / 1000:.1f}",
    "WrDurS": f"{WR['duration_s']:.1f}", "WrMin": f"{WR['duration_s'] / 60:.0f}", "WrWraps": f"{WR['ftm_wraps_crossed']}",
    "MacWrapMin": f"{WR['mac_wrap_s'] / 60:.1f}",
    "WrDelayLo": f"{min(wdl):.0f}", "WrDelayHi": f"{max(wdl):.0f}",
    "WrStepLo": f"{min(WR['per_node_1min_median_range_us'].values()):.0f}", "WrStepHi": f"{max(WR['per_node_1min_median_range_us'].values()):.0f}",
    "WrSpMed": f"{WR['spread_us']['median']}", "WrSpPn": f"{WR['spread_us']['p95']}", "WrSpPnn": f"{WR['spread_us']['p99']}", "WrSpMax": f"{WR['spread_us']['max']}",
    "WrJitMed": f"{WR['jitter_spread_us']['median']:.0f}", "WrJitPn": f"{WR['jitter_spread_us']['p95']:.0f}",
    "WrLost": f"{WR['sync_lost_checks']}", "WrChecks": f"{WR['sync_checks']:,}".replace(",", "{,}"),
    "WrLate": f"{WR['late_frames']}", "WrFrames": f"{WR['frames']:,}".replace(",", "{,}"),
    "WrYield": f"{100 * WR['full_slot_yield']:.0f}", "WrYieldLogged": f"{100 * WR['full_yield_of_logged']:.0f}", "WrErr": f"{WR['logger_errors']}",
    "JitOverClkLo": f"{jit['median'] / LIVE:.0f}", "JitOverClkHi": f"{WR['jitter_spread_us']['median'] / LIVE:.0f}",
    "McSD": f"{mc[0]:.3f}", "GpSD": f"{gp[0]:.3f}", "McMax": f"{mc[1]:.3f}", "GpMax": f"{gp[1]:.3f}",
    "ZeroLive": f"{sum(1 for x in data[4] if x == 0)}", "ZeroHold": f"{sum(1 for x in data[3] if x == 0)}",
}
# Regression guard: the values the author's brief quotes from the evidence files must be what the files say.
assert [N[k] for k in ("TrkDur", "TrkSlots", "DelayLo", "DelayHi", "JitMed", "RowMedMs", "XrowMed", "XrowOverJit", "XrowOverClk",
                       "DetLo", "DetHi", "SDthreeD", "LnineA", "LnineB", "LnineC", "LnineRes", "Quorum")] == [
    "296", "99.8", "1761", "1801", "33", "5.0", "468", "14", "197", "0.11", "0.13", "0.27", "150.10", "200.34", "249.44", "0.15", "0.999"], N
assert [N[k] for k in ("WrDurS", "WrWraps", "WrDelayLo", "WrDelayHi", "WrStepLo", "WrStepHi", "WrSpMed", "WrSpPn", "WrSpPnn", "WrSpMax",
                       "WrJitMed", "WrJitPn", "WrLost", "WrChecks", "WrLate", "WrFrames", "WrYield", "WrYieldLogged", "WrErr", "MacWrapMin")] == [
    "2398.8", "8", "2161", "2215", "25", "38", "85", "228", "344", "650", "67", "208", "0", "1{,}239", "53", "70{,}535", "69", "91", "97", "71.6"], N
assert (N["JitOverClkLo"], N["JitOverClkHi"], N["ZeroLive"], N["ZeroHold"]) == ("20", "40", "6", "14"), N
assert WR["duration_s"] < WR["mac_wrap_s"] and WR["period_us"] == 100000 and WR["nodes"] == [0, 1, 2, 3]   # MAC wrap NOT crossed; QVGA 10 frames/s, 4 nodes
assert all(c["paced_at_P"] for c in R4["per_camera"].values())
assert abs(TS["C_vs_F_clock_err_ratio_median"] - xrow["median"] / mc[1]) < 1e-6      # same ratio as analyze_tracking.py
(OUT / "numbers.tex").write_text("".join(f"\\newcommand{{\\{k}}}{{{v}}}\n" for k, v in N.items()))
cap = [  # Table 3 rows: quantity | tracking (VGA) | 40-min run (QVGA)
    (r"Trigger-to-stamp delay, per cam. (\textmu s)", f"{N['DelayLo']}--{N['DelayHi']}", f"{N['WrDelayLo']}--{N['WrDelayHi']}"),
    (r"Cross-cam. spread: med./p95 (\textmu s)", f"{N['RawMed']}/{N['RawPn']}", f"{N['WrSpMed']}/{N['WrSpPn']}"),
    (r"\quad fixed delay removed (\textmu s)", f"{N['JitMed']}/{N['JitPn']}", f"{N['WrJitMed']}/{N['WrJitPn']}"),
    (r"Slots from all cameras (\%)", N["TrkSlots"], f"{N['WrYieldLogged']}$^a$"),
    (r"Row offset $y\,t_\text{row}$: med./p95 (ms)", f"{N['RowMedMs']}/{N['RowPnMs']}", "--"),
    (r"Cross-cam. row-time diff. (\textmu s)", f"{N['XrowMed']}/{N['XrowPn']}", "--"),
]
(OUT / "table_capture.tex").write_text("\n".join(" & ".join(r) + r" \\" for r in cap) + "\n")
json.dump({"offset_stats": stats, "capture_numbers": N, "follower_means": {f"{k[0]}|{k[1]}": v for k, v in fmeans.items()},
           "three_cornered_hat": TCH, "nosync_drift_us_per_s": drift, "short_mcpwm_max": max(short)}, (OUT / "stats.json").open("w"), indent=1)
for k, s in stats.items():
    print(f"{k:45s} n={s['n']:4d} mean={s['mean']:+11.3f} sd={s['sd']:10.3f} p95={s['p95']:10.3f} p99={s['p99']:10.3f} max={s['max']:10.3f}")
print("figures ->", OUT)
