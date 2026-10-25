import numpy as np
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
stats["Live FTM, camera tracking load"] = summ(load)
# record L glitch check (a-priori 1-us filter; dataset timesync-2026-09-09/recheck_record_L.py)
import importlib.util as _ilu
_sp = _ilu.spec_from_file_location("recheckL", DATA / "timesync-2026-09-09/recheck_record_L.py"); _rL = _ilu.module_from_spec(_sp); _sp.loader.exec_module(_rL)
_Rf, _offL = _rL.offsets(_rL.FILTER_US)
assert sorted(round(x, 3) for x in _offL) == sorted(round(x, 3) for x in load), "record L changes under glitch filter"
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
# MCPWM tick 1 us (resolution_hz=1000000, archived strobe_gpio.c): follower minus reference = difference of two
# independent 1-us quantizers -> SD sqrt(2/12) (rev10, M7)
QDIFF = math.sqrt(2 / 12)
assert round(QDIFF, 2) == 0.41
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
# improvement factor TSF / live FTM max ~ 10^2 (NTP-style ratio dropped in rev10, M2)
assert round(stats["AP TSF"]["max"] / stats["Live FTM"]["max"]) == 108                     # "108x" in Sec III (with read-path caveat)
assert 100 < 182.188 / stats["Live FTM"]["max"] < 120
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
rec_rows.append(("Camera load", "L", load))
tex = []
SHOW = {"AP TSF": "AP TSF (API)", "NTP-style UDP": "NTP-style (naive)"}   # rev10 M2: TSF as exposed by ESP-IDF API
for label, rec, v in rec_rows:
    s = summ(v)
    tex.append(f"{SHOW.get(label, label)} & {rec} & {s['n']} & {('+' if s['mean'] >= 0 else '$-$') + fmt(abs(s['mean']))} & {fmt(s['sd'])} & {fmt(s['p95'])} & {fmt(s['max'])} \\\\")
out, prev = [], None
for l in tex:                                   # thin gap between sources
    k = l.split(" & ")[0].replace("Camera load", "Live FTM")
    if prev and k != prev: out.append("\\addlinespace[1pt]")
    out.append(l); prev = k
(OUT / "table_records.tex").write_text("\n".join(out) + "\n")
assert sum(1 for l, *_ in rec_rows if l == "Live FTM") == 4 and len(rec_rows) == 13, len(rec_rows)

plt.rcParams.update({"font.size": 8, "font.family": "serif", "font.serif": ["STIXGeneral", "DejaVu Serif"], "mathtext.fontset": "stix", "pdf.fonttype": 42, "axes.grid": True, "grid.alpha": .3})
# Printed size: every figure is drawn 3.5 in = 252 pt wide and set at \columnwidth = 249 pt,
# so printed pt = fig pt x 0.9881. FSMIN is the smallest size that still prints >= 7 pt.
FSMIN = 7.1
assert FSMIN * (249 / 252) >= 7.0

# Fig 2: source comparison, |offset| log scale
fig, ax = plt.subplots(figsize=(3.5, 1.6))
ax.boxplot(data, whis=(0, 100), widths=.55, medianprops=dict(color="k"))
ax.set_yscale("symlog", linthresh=0.1, linscale=0.6); ax.set_ylim(-0.03, 1.2e6); ax.set_yticks([0, 1, 1e2, 1e4], ["0", "$10^{0}$", "$10^{2}$", "$10^{4}$"]); ax.set_ylabel(r"|follower $-$ reference| ($\mu$s)")
ax.set_xticks(range(1, 6), ["No\nsync", "NTP\nstyle", "AP\nTSF", "FTM\nhold", "Live\nFTM"])
ax.axhline(1000, ls="--", lw=.8, color="0.4"); ax.text(5.42, 1000, "1 ms", ha="right", va="bottom", fontsize=8, color="0.3")
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


# Fig 1 (system + capture timing) is drawn at the end, after the capture numbers N exist.



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
# ---- Revision 6: software vs hardware capture path (MCPWM-latched VSYNC stamp + GPTimer-started FREX write) ----
import re
CAMP = DATA / "campaign-2026-10-17"           # rev12: capture layer, latency and arm on the current build (fw 842af0eb)
AB = json.load((CAMP / "ab/tracking_ab.stats.json").open())
ABHW = json.load((CAMP / "ab/tracking-hw.json").open())
ABSW = json.load((CAMP / "ab/tracking-sw.json").open())
WH = json.load((DATA / "wrap-run/wrap-40min-hw.jsonl.stats.json").open())
assert (ABSW["hw"], ABHW["hw"], ABSW["dur"], ABHW["dur"], ABSW["status"]["resolution"]) == ("off", "on", 90.0, 90.0, "vga")
assert all(AB[p]["cameras"] == [0, 1, 2, 3] and all(round(f) == 35 for f in AB[p]["per_camera_fps"].values()) for p in ("sw", "hw"))
def kv(s, key):   # "key=a..b mean=m" / "key=n" fields of the node counter lines
    m = re.search(rf"\b{key}=(-?\d+)(?:\.\.(-?\d+))?", s); return tuple(int(g) for g in m.groups() if g is not None)
hs = [l for v in ABHW["node_stats"].values() for l in v if l.startswith("hwstamp")]
ht = [l for v in ABHW["node_stats"].values() for l in v if l.startswith("hwtrig")]
assert len(hs) == len(ht) == 4 and all(kv(l, "on") == (1,) for l in hs + ht)
lag = [kv(l, "sw_lag_us") for l in hs]; lagm = [kv(l, "mean")[0] for l in hs]
ok = [kv(l, "ok")[0] for l in ht]; lt = [kv(l, "late")[0] for l in ht]; done = [kv(l, "done_us") for l in ht]
assert sum(kv(l, "misses")[0] for l in hs) == 0 and sum(kv(l, k)[0] for l in ht for k in ("nack", "timeout")) == 0
# node-2 delay per 5-min bin while it reported transient FTM sync gaps (hardware 40-min run)
import collections
by5 = collections.defaultdict(list)
WL = [json.loads(l) for l in (DATA / "wrap-run/wrap-40min-hw.jsonl").open()]
F = [x for x in WL if x["kind"] == "f"]; tt0 = min(x["tick"] for x in F)
for x in F:
    if x["node"] == 2 and x["cap"] - x["tick"] < 10_000: by5[(x["tick"] - tt0) // 300_000_000].append(x["cap"] - x["tick"])
n2 = [st.median(v) for v in by5.values()]
def g(x): return f"{x:.0f}" if x >= 10 or x == int(x) else f"{x:.1f}"
s, h = AB["sw"], AB["hw"]
N.update({
    "AbDur": f"{ABHW['dur']:.0f}", "AbFps": f"{round(st.mean(h['per_camera_fps'].values()))}",
    "SwDelayLo": g(min(s["per_camera_offset_from_slot_us"].values())), "SwDelayHi": g(max(s["per_camera_offset_from_slot_us"].values())),
    "HwDelayLo": g(min(h["per_camera_offset_from_slot_us"].values())), "HwDelayHi": g(max(h["per_camera_offset_from_slot_us"].values())),
    "SwSpMed": g(s["spread_us"]["median"]), "SwSpPn": g(s["spread_us"]["p95"]), "SwSpMax": g(s["spread_us"]["max"]),
    "HwSpMed": g(h["spread_us"]["median"]), "HwSpPn": g(h["spread_us"]["p95"]), "HwSpPnn": g(h["spread_us"]["p99"]), "HwSpMax": g(h["spread_us"]["max"]),
    "SwJitMed": g(s["jitter_fixed_removed_us"]["median"]), "SwJitPn": g(s["jitter_fixed_removed_us"]["p95"]),
    "HwJitMed": g(h["jitter_fixed_removed_us"]["median"]), "HwJitPn": g(h["jitter_fixed_removed_us"]["p95"]), "HwJitPnn": g(h["jitter_fixed_removed_us"]["p99"]),
    "SwYield": f"{100 * s['full_yield']:.1f}", "HwYield": f"{100 * h['full_yield']:.1f}",
    "LagLo": f"{min(a for a, _ in lag)}", "LagHi": f"{max(b for _, b in lag)}", "LagMeanLo": f"{min(lagm)}", "LagMeanHi": f"{max(lagm)}",
    "TrigOkLo": f"{min(ok):,}".replace(",", "{,}"), "TrigOkHi": f"{max(ok):,}".replace(",", "{,}"), "TrigLateLo": f"{min(lt)}", "TrigLateHi": f"{max(lt)}",
    "TrigDoneLo": f"{min(a for a, _ in done)}", "TrigDoneHi": f"{max(b for _, b in done)}",
    "WhDelayLo": g(min(WH["per_node_median_delay_us"].values())), "WhDelayHi": g(max(WH["per_node_median_delay_us"].values())),
    "WhSpMed": g(WH["spread_us"]["median"]), "WhSpPn": g(WH["spread_us"]["p95"]), "WhSpPnn": g(WH["spread_us"]["p99"]), "WhSpMax": g(WH["spread_us"]["max"]),
    "WhJitMed": g(WH["jitter_spread_us"]["median"]), "WhJitPn": g(WH["jitter_spread_us"]["p95"]),
    "WhStepLo": g(min(WH["per_node_1min_median_range_us"].values())), "WhStepHi": g(max(WH["per_node_1min_median_range_us"].values())),
    "WhMin": f"{WH['duration_s'] / 60:.0f}", "WhWraps": f"{WH['ftm_wraps_crossed']}", "WhLate": f"{WH['late_frames']}", "WhFrames": f"{WH['frames']:,}".replace(",", "{,}"),
    "WhLost": f"{WH['sync_lost_checks']}", "WhLostTwo": f"{WH['sync_lost_by_node']['2']}", "WhChecks": f"{WH['sync_checks']:,}".replace(",", "{,}"),
    "WhAgeS": f"{WH['max_sync_age_ms'] / 1000:.1f}", "WhNtwoLo": g(min(n2)), "WhNtwoHi": g(max(n2)),
    "WhYield": f"{100 * WH['full_slot_yield']:.0f}", "WhYieldLogged": f"{100 * WH['full_yield_of_logged']:.0f}", "WhErr": f"{WH['logger_errors']}",
})
# rev12: row-readout offsets from the current-build hardware-path recording (vskip 4, t_row checked against HREF)
CS = json.load((CAMP / "capture.stats.json").open())["hw"]
row, xrow = CS["C_marker_row_time_us"], CS["C_cross_camera_row_time_diff_us"]
assert CS["row_period_ns"] == 25750 and abs(CS["C_full_frame_row_span_us"] - 12360) < 1 and CS["inferred_fps"] == 35
N.update({"RowUs": f"{CS['row_period_ns'] / 1000:.2f}", "RowSpanMs": f"{CS['C_full_frame_row_span_us'] / 1000:.2f}",
          "RowMedMs": f"{row['median'] / 1000:.1f}", "XrowMed": f"{xrow['median']:.0f}", "RowDur": f"{CS['duration_s']:.0f}"})
# Regression guard: values quoted in the revision-6 brief must be what the files say.
assert [N[k] for k in ("AbDur", "AbFps", "SwDelayLo", "SwDelayHi", "HwDelayLo", "HwDelayHi", "SwSpMed", "SwSpPn", "SwSpMax", "HwSpMed", "HwSpPn", "HwSpMax",
                       "SwJitMed", "SwJitPn", "HwJitMed", "HwJitPn", "HwJitPnn", "SwYield", "HwYield")] == [
    "90", "35", "1228", "1767", "641", "1179", "540", "573", "925", "538", "540", "543", "14", "198", "1.4", "2.9", "3.7", "99.6", "100.0"], N
assert [N[k] for k in ("LagLo", "LagHi", "LagMeanLo", "LagMeanHi", "TrigLateLo", "TrigLateHi")] == ["134", "350", "138", "140", "0", "0"], N
assert all(3_150 < x < 3_250 for x in ok)                                       # ~3,180 per node over the 90-s arm
# the fixed per-camera delays are ordered by each camera's own exposure setting (operator-tuned, tune-before.txt, nodes 0..3)
EXPO = [int(re.search(r"exposure=(\d+)", l).group(1)) for l in (CAMP / "tune-before.txt").read_text().splitlines() if l.startswith("tune:")]
_hd = h["per_camera_offset_from_slot_us"]
assert len(EXPO) == 4 and [EXPO[i] for i in sorted(range(4), key=lambda i: _hd[str(i)])] == sorted(EXPO)
N.update({"ExpoLo": str(min(EXPO)), "ExpoHi": str(max(EXPO))})
assert [N[k] for k in ("WhDelayLo", "WhDelayHi", "WhSpMed", "WhSpPn", "WhSpPnn", "WhSpMax", "WhJitMed", "WhJitPn", "WhStepLo", "WhStepHi",
                       "WhWraps", "WhLate", "WhFrames", "WhLost", "WhLostTwo", "WhChecks", "WhAgeS", "WhNtwoLo", "WhNtwoHi")] == [
    "1600", "1629", "35", "69", "148", "342", "30", "68", "6.5", "15", "8", "0", "84{,}315", "12", "11", "1{,}335", "6.8", "1599", "1602"], N
assert N["WhMin"] == N["WrMin"] == "40"
# ---- Revision 9 (journal review r8) ----
# M2 two-term budget: clock term = camera-load GPIO record L (only clock evidence with cameras running);
# capture term = HW delay-removed spread. Different builds; no external skew on the capture build.
LD = stats["Live FTM, camera tracking load"]
hdl = h["per_camera_offset_from_slot_us"].values()
# M3 FREX global reset: effective per-row instant moves 1/2..1 t_row per row -> magnitudes are half..full of the y*t_row values
# M4 ratios: SD and p95 lead, max secondary; backend esp_timer vs GPTimer with robust spreads too
def iqr(v): q = st.quantiles(v, n=4, method="inclusive"); return q[2] - q[0]
def madsd(v): m = st.median(v); return 1.4826 * st.median([abs(x - m) for x in v])
bv = {m: [float(r["offset_us"]) for r in rows if BACKEND_RUN in r["run"] and r["method"] == m] for m in ("esp:ftm", "gpt:ftm")}
TS_, LF_ = stats["AP TSF"], stats["Live FTM"]
N.update({
    "LoadSD": f"{LD['sd']:.2f}", "LoadMax": f"{LD['max']:.2f}", "LoadN": f"{LD['n']}",
    "HwDelayRange": f"{max(hdl) - min(hdl):.1f}",
    "RowHalfMs": f"{row['median'] / 2000:.1f}", "XrowHalf": f"{xrow['median'] / 2:.0f}",
    "TsfSdRatio": f"{TS_['sd'] / LF_['sd']:.0f}", "TsfPnRatio": f"{TS_['p95'] / LF_['p95']:.0f}",
    "TsfMaxRatio": f"{TS_['max'] / LF_['max']:.0f}", "TsfN": f"{TS_['n']}", "LiveN": f"{LF_['n']}",
    "BkSdRatio": f"{st.pstdev(bv['esp:ftm']) / st.pstdev(bv['gpt:ftm']):.0f}",
    "BkIqrRatio": f"{iqr(bv['esp:ftm']) / iqr(bv['gpt:ftm']):.0f}", "BkMadRatio": f"{madsd(bv['esp:ftm']) / madsd(bv['gpt:ftm']):.0f}",
    "BkN": f"{len(bv['esp:ftm'])}",
    "RefSdLo": f"{min(ref_sd):.2f}", "RefSdHi": f"{max(ref_sd):.2f}", "LiveSD": f"{LF_['sd']:.2f}",
})
assert N["LiveSD"] == "0.67"
# ---- Revision 10 (journal review r9) ----
L4 = [summ([float(r["offset_us"]) for r in rows if r["run"].endswith(run)]) for run in LIVE4]          # M7: four live-FTM records
HO = stats["FTM holdover"]
fire = [kv(l, "fire_us") for l in ht]
N.update({
    "LiveFourSdLo": f"{min(x['sd'] for x in L4):.2f}", "LiveFourSdHi": f"{max(x['sd'] for x in L4):.2f}",
    "LiveFourMaxLo": f"{min(x['max'] for x in L4):.2f}", "LiveFourMaxHi": f"{max(x['max'] for x in L4):.2f}",
    "QuantDiffSd": f"{QDIFF:.2f}", "HoldSD": f"{HO['sd']:.2f}", "HoldMax": f"{HO['max']:.2f}", "LiveMax": f"{LF_['max']:.2f}",
    "FireLo": f"{min(a for a, _ in fire)}", "FireHi": f"{max(b for _, b in fire)}",
})
assert [N[k] for k in ("LiveFourSdLo", "LiveFourSdHi", "LiveFourMaxLo", "LiveFourMaxHi", "QuantDiffSd", "HoldSD", "HoldMax",
                       "LiveMax", "FireLo", "FireHi")] == ["0.60", "0.67", "1.56", "1.94", "0.41", "0.70", "1.94", "1.69",
                                                                 "2", "4"], N
assert abs(HO["sd"] - LF_["sd"]) < 0.1 and HO["n"] == 357        # M3: holdover ~ live over 60 s
# rev12 R5: per-record holdover range, so the text compares record ranges with record ranges (Table 2 rows)
_HOr = [summ([float(r["offset_us"]) for r in rows if r["run"] == run and r["method"] == "mcpwm:frozen"])
        for run in sorted({r["run"] for r in rows if CAMPAIGN in r["run"] and r["method"] == "mcpwm:frozen"})]
assert len(_HOr) == 2 and sum(x["n"] for x in _HOr) == HO["n"]
N.update({"HoldSdLo": f"{min(x['sd'] for x in _HOr):.2f}", "HoldSdHi": f"{max(x['sd'] for x in _HOr):.2f}"})
assert [N[k] for k in ("LoadSD", "LoadMax", "LoadN", "HwDelayRange", "RowHalfMs", "XrowHalf", "TsfSdRatio", "TsfPnRatio", "TsfMaxRatio",
                       "TsfN", "LiveN", "BkSdRatio", "BkIqrRatio", "BkMadRatio", "BkN", "RefSdLo", "RefSdHi")] == [
    "0.89", "2.75", "180", "538.0", "1.6", "1205", "32", "30", "108", "358", "180", "50", "24", "25", "75", "0.36", "0.46"], N
assert len({r["run"] for r in rows if CAMPAIGN in r["run"] and r["method"] == "mcpwm:tsf"}) == 2                  # AP TSF: two records
assert len({r["run"] for r in rows if CAMPAIGN in r["run"] and r["method"] == "mcpwm:ftm"}) == 1                  # live FTM: one record
# rev12: on the current build the spread is dominated by fixed per-camera delays (~HwDelayRange); the row term is
# compared with the delay-removed spread (the delay-consistency measure) and stated against the raw spread as a ratio
N.update({"XrowOverSp": f"{xrow['median'] / 2 / h['spread_us']['p95']:.1f}", "XrowOverJitHw": f"{xrow['median'] / 2 / h['jitter_fixed_removed_us']['p95']:.0f}"})
assert xrow["median"] / 2 > 2 * h["spread_us"]["p95"] and xrow["median"] / 2 > 100 * h["jitter_fixed_removed_us"]["p95"]
assert h["spread_us"]["median"] - (max(hdl) - min(hdl)) < 1                       # HW spread median ~ the fixed per-camera delay difference
assert s["jitter_fixed_removed_us"]["median"] > 8 * LIVE and h["jitter_fixed_removed_us"]["median"] < LIVE   # SW stamp lag >> clock, HW below it
assert WH["sync_lost_by_node"] == {"1": 0, "2": 11, "3": 1} and WH["period_us"] == 100000 and WH["nodes"] == [0, 1, 2, 3] and len(n2) == 8
assert h["jitter_fixed_removed_us"]["p95"] < s["jitter_fixed_removed_us"]["median"] and WH["jitter_spread_us"]["median"] < WR["jitter_spread_us"]["median"]   # direction of every claim
# ---- Robot-arm reference (dataset arm-reference/arm-reference.json) ----
AR = json.load((CAMP / "arm-reference.json").open())
AR2 = json.load((CAMP / "arm-reference-2.json").open())["summary"]          # repeat after re-homing, same calibration
APR = json.load((CAMP / "acc_arm_pairs.json").open())                      # pair-length check (thesis accuracy_arm.py convention)
ARs = AR["summary"]; ARok = [r for r in AR["rows"] if "err" in r]
assert ARs["n_ok"] == len(ARok) == 36 and ARs["n_fail"] == 0 and ARs["n_poses"] == 18 and ARs["repeats"] == 2
_e = sorted(r["err_norm"] for r in ARok)
assert abs(np.median(_e) - ARs["err_median_mm"]) < 1e-9 and abs(_e[-1] - ARs["err_max_mm"]) < 1e-9
N.update({
    "ArmPoses": str(ARs["n_poses"]), "ArmVisits": str(ARs["n_ok"]),
    "ArmCalRms": f"{ARs['calibration']['rms_mm']:.1f}", "ArmCalN": str(ARs["calibration"]["points"]),
    "ArmErrMed": f"{ARs['err_median_mm']:.1f}", "ArmErrRms": f"{ARs['err_rms_mm']:.1f}", "ArmErrMax": f"{ARs['err_max_mm']:.1f}",
    "ArmStaticSd": f"{ARs['static_ir_sd_median_mm']:.2f}", "ArmRepeat": f"{ARs['repeat_ir_dist_median_mm']:.1f}",
})
N.update({"ArmErrMedB": f"{AR2['err_median_mm']:.1f}", "ArmErrMaxB": f"{AR2['err_max_mm']:.1f}", "ArmRepeatB": f"{AR2['repeat_ir_dist_median_mm']:.1f}",
          "PairN": str(APR["pairs"]), "PairPoses": str(APR["poses"]), "PairRms": f"{APR['err_rms_mm']:.1f}", "PairMed": f"{APR['abs_median_mm']:.1f}",
          "PairLo": f"{APR['len_min_mm']:.0f}", "PairHi": f"{APR['len_max_mm']:.0f}"})
# Follow-up split (2026-10-19, arm_split.py): static Calibrator logged at every visit; restarts with
# the tracker's online per-camera pixel correction on vs off.
AS = json.load((CAMP / "arm-split.stats.json").open())
_on, _off = AS["arm-backlash.json"], AS["arm-backlash-biasoff.json"]
# Arm return from the correction-on run: the condition whose residual is being explained (referee B1).
N.update({"SplitArmA": f"{_on['A']['arm_median_mm']:.1f}", "SplitArmB": f"{_on['B']['arm_median_mm']:.1f}",
          "SplitArmMax": f"{max(_on['A']['arm_max_mm'], _on['B']['arm_max_mm']):.1f}",
          "SplitShift": f"{st.median([AS['settle']['bias_on']['shift_first8s_mm'], AS['settle']['bias_on_2']['shift_first8s_mm']]):.0f}",
          "SplitShiftOff": f"{AS['settle']['bias_off']['shift_first8s_mm']:.1f}",
          "SplitErrOff": f"{_off['B']['abs_err_median_mm']:.1f}",
          "SplitPairRms": f"{_off['pairs']['rms_mm']:.1f}"})
# claims: the arm returns sub-mm; bias learning moves the world by >10 mm after a start and not when off
assert all(r[p]["arm_median_mm"] < 0.5 for r in (_on, _off) for p in "AB")
assert AS["settle"]["bias_off"]["shift_first8s_mm"] < 1.0 < 10.0 < min(AS["settle"][k]["shift_first8s_mm"] for k in ("bias_on", "bias_on_2"))
assert all(AS["settle"][k]["settle_s"] < 180 for k in ("bias_on", "bias_on_2"))   # "over ~2-3 minutes"
assert _off["B"]["abs_err_median_mm"] < 1.5
# Reference objects (rig/rigid_bodies.json as used for every 2026-10-19 run): static calibrator frame and the
# Dobot end-effector marker; inter-LED distance range of the marker for the setup description.
_RB = {b["name"]: np.array(b["points_mm"]) for b in json.load((CAMP / "rig" / "rigid_bodies.json").open())}
_mk = [np.linalg.norm(a - b) for a, b in itertools.combinations(_RB["dobot-marker-B"], 2)]
_cal = sorted(np.linalg.norm(a - b) for a, b in itertools.combinations(_RB["Calibrator"], 2))
assert abs(_cal[0] - 149.5) < 1e-9 and abs(_cal[1] - 196.5) < 1e-9
N.update({"MkLo": f"{min(_mk):.0f}", "MkHi": f"{max(_mk):.0f}", "MkLeds": str(len(_RB["dobot-marker-B"]))})
# Length accuracy vs the Dobot's step-count kinematics (2026-10-19, arm_length.py + arm_length_stats.py): every pair of
# reached poses, tracked distance minus arm distance; no registration or scale fit enters.
LA = json.load((CAMP / "arm-length.stats.json").open())
N.update({"LenRms": f"{LA['rms_mm']:.1f}", "LenRel": f"{LA['rel_rms_pct']:.1f}", "LenPairs": str(LA["pairs"]),
          "LenPoses": str(LA["poses"]), "LenLo": f"{LA['len_min_mm']:.0f}", "LenHi": f"{LA['len_max_mm']:.0f}",
          "LenScale": f"{LA['scale_err_pct']:.2f}", "LenPn": f"{LA['abs_p95_mm']:.1f}",
          "LenCmd": str(LA["attempted"]), "LenScaleSe": f"{LA['scale_se_pct']:.2f}"})
assert 1.0 < LA["rms_mm"] < 4.0 and abs(LA["scale_err_pct"]) < 0.5 and LA["pairs"] > 500
# Length test against a known object (2026-10-19, walk/walk_lengths.py): the calibrator (149.5 x 196.5 mm
# right triangle, design lengths, caliper-checked) carried through the capture area, bias learning off.
WK = json.load((CAMP / "walk" / "walk-lengths.json").open())
_C = np.array([c["c"] for c in WK["cells"]]); _E = np.array([c["err"] for c in WK["cells"]])
_r = np.linalg.norm(_C - np.median(_C, 0), axis=1)
_rin = lambda m: float(np.sqrt((_E[m] ** 2).mean()))
N.update({"WalkRms": f"{WK['cell_side_err_rms_mm']:.1f}", "WalkRel": f"{WK['cell_rel_rms_pct']:.1f}",
          "WalkCells": str(WK["cells_100mm"]), "WalkPn": f"{WK['cell_side_err_abs_p95_mm']:.1f}",
          "WalkSideA": "149.5", "WalkSideB": "196.5",   # calibrator design (rigid_bodies.json "Calibrator")
          # radii about the median of the occupied cells (the walked path), m
          "WalkIn": f"{_rin(_r < 400):.1f}", "WalkOut": f"{_rin(_r >= 800):.1f}", "WalkInR": "0.4", "WalkOutR": "0.8",
          "WalkSpan": "--".join(f"{v / 1000:.1f}" for v in sorted(np.ptp(_C, 0))[::2])})
assert 1.0 < WK["cell_side_err_rms_mm"] < 4.0 and _rin(_r < 400) < _rin(_r >= 800)
# claims: absolute (self-registered) error is a few mm and of the same size as the arm's own return-to-pose spread,
# the IR point is >10x steadier than that, and the calibration-free pair lengths agree to ~2 mm RMS
assert 2.0 < ARs["err_median_mm"] < 6.0 and 2.0 < AR2["err_median_mm"] < 6.0
assert ARs["static_ir_sd_median_mm"] * 10 < ARs["err_median_mm"]
assert 0.5 * ARs["err_median_mm"] < ARs["repeat_ir_dist_median_mm"] < 1.5 * ARs["err_median_mm"]
assert APR["poses"] == 17 and APR["pairs"] == 136 and APR["err_rms_mm"] < ARs["err_rms_mm"] / 2
# ---- Dobot dynamic sweeps (campaign 2026-10-17: 15/50/100 % sweeps + wide 100 % yaw sweep, 4-LED marker) ----
# Each sweep set is analysed against its own static distances (dynamic_n.py); rows pooled for the figure/macros.
TRIM_NS = 300_000_000                        # sweep ends dropped (acceleration), printed as \DynTrim
N["DynTrim"] = f"{TRIM_NS / 1e9:.1f}"
_mv, _inseg, _rows_e, _noise_l, _vmaxs = [], [], [], [], []
for _dyn, _pai, _sg in (("dynamic.json", "paired.json", ("seg-speed15.json", "seg-speed50.json", "seg-speed100.json")),
                        ("dynamic-wide.json", "paired-wide.json", ("seg-speed100-wide.json",))):
    DY = json.load((CAMP / _dyn).open()); PA = json.load((CAMP / _pai).open())
    _segs = [g for f in _sg for g in json.load((CAMP / f).open())["segments"]]
    _ref = np.array(DY["summary"]["static_d_mm"])
    _in = [r for r in DY["rows"]["1.0"] if any(g["t0_ns"] + TRIM_NS <= r["host_ns"] <= g["t1_ns"] - TRIM_NS for g in _segs)]
    _m = [r for r in _in if 20 < r["speed_mm_s"] < 800]
    _inseg += _in; _mv += _m; _rows_e += [np.abs(np.array(r["d_mm"]) - _ref) for r in _m]
    _noise_l += DY["summary"]["static_d_sd_mm"]; _vmaxs.append(max(v["speed_max_mm_s"] for v in PA["1.0"].values()))
    assert DY["summary"]["body"] == "dobot-marker-B" and len(DY["summary"]["pairs"]) == 6
_E = np.array(_rows_e); _e = _E.ravel(); _slot = _E.max(axis=1)
_vmax = max(r["speed_mm_s"] for r in _mv)          # fastest included slot (central-difference tracked speed)
assert _vmax > max(_vmaxs)                     # paired.json bins report per-bin speeds, lower than the slot maximum
_speed = np.array([r["speed_mm_s"] for r in _mv])
_noise = float(np.median(_noise_l))
_hi = _speed >= 500
N.update({
    "DynN": f"{len(_mv):,}".replace(",", "{,}"), "DynVmax": f"{math.floor(_vmax):.0f}", "DynErrMed": f"{np.median(_e):.1f}", "DynErrPn": f"{np.percentile(_e, 95):.1f}",
    "DynInSeg": f"{len(_inseg):,}".replace(",", "{,}"), "DynExcl": str(len(_inseg) - len(_mv)),
    "DynBig": str(int((_slot > 5).sum())), "DynErrMax": f"{_slot.max():.1f}",
    "DynNoise": f"{_noise:.2f}", "DynSkewDetMs": f"{_noise / _vmax * 1e3:.1f}",
    "DynHiN": str(int(_hi.sum())), "DynHiMed": f"{np.median(_E[_hi]):.1f}", "DynHiPn": f"{np.percentile(_E[_hi], 95):.1f}",
    "DynLeds": "4",
})
# claims: moving inter-LED distance error stays within the static noise scale up to the top speed
assert np.median(_e) < 1.0 and np.percentile(_e, 95) < 2.0 and _vmax > 500 and _hi.sum() >= 100
assert np.median(_E[_hi]) < 1.0 and np.percentile(_E[_hi], 95) < 2.0
assert 0.5 < _noise / _vmax * 1e3 < 5
# ---- Fig. 3: rig top view (calibrated geometry) + moving distance error vs speed ----
_rig = CAMP / "rig"
_ex = json.load((_rig / "extrinsics.json").open())["extrinsics"]
_cal = AR["summary"]["calibration"]
_Rr, _tr = np.array(_cal["r"]), np.array(_cal["t"])
_w = lambda p: (_Rr @ np.array(p, float) + _tr) / 1000          # robot mm -> world m
_held = [r for r in AR["rows"] if "ir_world" in r]
fig = plt.figure(figsize=(3.5, 1.95))
_gs = fig.add_gridspec(2, 2, width_ratios=[1.1, 1.2], height_ratios=[0.75, 1.15])
a0 = fig.add_subplot(_gs[:, 0]); a2 = fig.add_subplot(_gs[0, 1]); a1 = fig.add_subplot(_gs[1, 1])
# (b) reference-object design, from the CAD and rigid_bodies.json: marker top view in its tool-socket frame
# (outline = STL triangles, dots = LED dome centres) and the calibrator triangle beside it, same mm scale.
_mk_dir = CAMP / "rig"  # marker CAD (STL + LED dome centres), copied into the public dataset
_tri = np.fromfile(_mk_dir / "ee_marker_B.stl", dtype=np.dtype([("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")]), offset=84)["v"]
_leds = np.array(list(json.load((_mk_dir / "ee_marker_B_leds.json").open())["leds_dome_centre"].values()))
from matplotlib.collections import PolyCollection
a2.add_collection(PolyCollection(_tri[:, :, :2], fc="0.6", ec="0.45", lw=.15, zorder=1))
a2.plot(_leds[:, 0], _leds[:, 1], "o", ms=3, mfc="#d62728", mec="k", mew=.3, zorder=3)
_cpts = np.array(_RB["Calibrator"])[:, :2] + [195, -100]          # placed right of the marker, same scale
a2.fill(*_cpts.T, fc="none", ec="k", lw=.6, zorder=2); a2.plot(*_cpts.T, "o", ms=3, mfc="#d62728", mec="k", mew=.3, zorder=3)
a2.plot([-50, 50], [-75, -75], "k-", lw=.8, solid_capstyle="butt")
a2.text(0, -85, "100 mm", fontsize=FSMIN, ha="center", va="top")
a2.text(45, 64, "marker", fontsize=FSMIN, ha="center", va="bottom"); a2.text(_cpts[:, 0].mean() + 25, _cpts[0, 1] - 22, "calibrator", fontsize=FSMIN, ha="center", va="top")
a2.set_xlim(-100, 360); a2.set_ylim(-142, 122); a2.set_aspect("equal"); a2.set_axis_off()
a2.text(0.0, .985, "(b)", transform=a2.transAxes, fontsize=FSMIN, va="top")
_lab = {0: (-10, -4), 1: (2, 6), 2: (-11, 2), 3: (2, 6)}
for e in _ex:
    R, t = np.array(e["R"]), np.array(e["t"]); C = -R.T @ t / 1000; z = R.T @ np.array([0, 0, 1.0]); u = z[:2] / np.hypot(*z[:2])
    a0.annotate("", C[:2] + .6 * u, C[:2], arrowprops=dict(arrowstyle="-|>", lw=.6, mutation_scale=6, color="k"))
    a0.plot(*C[:2], "s", ms=3.5, color="k", zorder=3)
    a0.annotate(f"C{e['camera_id']}", C[:2], xytext=_lab[e["camera_id"]], textcoords="offset points", fontsize=FSMIN, ha="center")
# arm working area used (calibration grid r 175-275 mm, yaw +-35 deg) as a wedge, sweeps as lines
th = np.radians(np.linspace(-35, 35, 30))
ring = np.vstack([[_w([r * np.cos(a), r * np.sin(a), 50]) for a in th] for r in (175,)] + [[_w([275 * np.cos(a), 275 * np.sin(a), 50]) for a in th[::-1]]])
a0.fill(ring[:, 0], ring[:, 1], fc="#9ec5e8", ec="#1f77b4", lw=.6, zorder=1)
a0.plot(*_w([0, 0, 0])[:2], "o", ms=3, mfc="0.6", mec="k", mew=.4, zorder=2)
a0.annotate("Dobot", _w([0, 0, 0])[:2], xytext=(0, -9), textcoords="offset points", fontsize=FSMIN, ha="center")
_cc = np.array([-np.array(e["R"]).T @ np.array(e["t"]) / 1000 for e in _ex])[:, :2]       # limits from the actual rig
a0.set_xlim(_cc[:, 0].min() - .7, _cc[:, 0].max() + .7); a0.set_ylim(_cc[:, 1].min() - .9, _cc[:, 1].max() + .6); a0.set_aspect("equal")
a0.set_xlabel("x (m)", fontsize=FSMIN); a0.set_ylabel("y (m)", fontsize=FSMIN); a0.tick_params(labelsize=FSMIN)
a0.text(.04, .97, "(a)", transform=a0.transAxes, fontsize=FSMIN, va="top")
_v = _speed; _de = _e
_v3 = np.repeat(_v, _E.shape[1])
_bins = np.arange(0, 800, 100); _ok = [((_v >= lo) & (_v < lo + 100)).sum() >= 30 for lo in _bins]   # >= 30 slots per bin
_q = np.array([np.percentile(_de[(_v3 >= lo) & (_v3 < lo + 100)], [25, 50, 75]) for lo, k in zip(_bins, _ok) if k])
_c = np.array([lo + 50 for lo, k in zip(_bins, _ok) if k])
a1.plot(_v3, _de, ".", ms=.6, alpha=.12, color="0.65", rasterized=True, zorder=2)
a1.fill_between(_c, _q[:, 0], _q[:, 2], color="#1f77b4", alpha=.45, lw=0, zorder=3)
a1.plot(_c, _q[:, 1], "-o", ms=2.4, lw=1.1, color="#14537f", mec="white", mew=.3, zorder=4)
a1.set_xlabel("marker speed (mm/s)", fontsize=FSMIN); a1.set_ylabel(r"$|\Delta d|$ (mm)", fontsize=FSMIN); a1.tick_params(labelsize=FSMIN)
a1.set_ylim(-.05, float(np.max(_de)) * 1.04); a1.set_xlim(0, 750)   # rev12 R2-M3: limits from the data, nothing clipped
assert np.max(_v3) < 750   # fastest included slot is \DynVmax mm/s; the axis still shows every point
a1.text(.97, .97, "(c)", transform=a1.transAxes, fontsize=FSMIN, va="top", ha="right")
fig.tight_layout(pad=.2, w_pad=1.6, h_pad=.4); fig.savefig(OUT / "rig_dynamic.pdf", dpi=300); _fig_arm = fig
# Delay-removed spread uses capture stamps at 1-us resolution (capture_us integer): the range of 4
# independent uniform quantisation errors has E = (n-1)/(n+1) = 0.6 us (n=4).
assert all(isinstance(json.loads(m.data)["capture_us"], int) for _, _, m in __import__("itertools").islice(
    __import__("mcap.reader", fromlist=["make_reader"]).make_reader((DATA / "tracking/ab/tracking-hw.mcap").open("rb")).iter_messages(topics=["/mocap/centroids"]), 200))
N["QuantRange"] = f"{(4 - 1) / (4 + 1):.1f}"
assert N["QuantRange"] == "0.6" and float(N["QuantRange"]) < float(N["HwJitMed"])
# End-to-end latency of the hardware-path A/B run (dataset tracking/ab/latency_ab.stats.json, latency_probe.py):
# times after the frame start (VSYNC), medians unless named. "Host" = last camera of the slot at the host,
# excluding the unknown fastest one-way Wi-Fi delay (the probe measures delivery as excess over its minimum).
LATF = json.load((CAMP / "latency/fast/latency-lowlat-vskip4.stats.json").open())       # rev12: shipped low-latency mode, vskip 4
LAT = next(r for r in LATF["rates"] if r["fps"] == 35)
N.update({
    "EeSlotMs": f"{LAT['slot_period_us'] / 1e3:.1f}",
    "EeReadyMs": f"{LAT['frame_ready_us']['median'] / 1e3:.1f}",
    "EeProcUs": f"{LAT['preprocess_us']['median'] + LAT['detection_us']['median']:.0f}",
    "EeSendMs": f"{LAT['send_after_capture_us']['median'] / 1e3:.1f}",
    "EeHostMed": f"{LAT['capture_to_last_camera_at_host_us']['median'] / 1e3:.1f}",
    "EeHostPn": f"{LAT['capture_to_last_camera_at_host_us']['p95'] / 1e3:.1f}",
})
N.update({"EeDelivMed": f"{LAT['delivery_excess_us']['median'] / 1e3:.1f}", "EeSpreadMed": f"{LAT['slot_arrival_spread_us']['median'] / 1e3:.1f}",
          "EeRateLo": str(min(r["fps"] for r in LATF["rates"])), "EeHostLo": f"{min(r['capture_to_last_camera_at_host_us']['median'] for r in LATF['rates']) / 1e3:.1f}",
          "EeHostHiFps": str(max(LATF["rates"], key=lambda r: r["capture_to_last_camera_at_host_us"]["median"])["fps"]),
          "EeHostHi": f"{max(r['capture_to_last_camera_at_host_us']['median'] for r in LATF['rates']) / 1e3:.1f}",
          "EeRttMed": f"{np.median([v['median'] for v in LATF['icmp_rtt_ms'].values()]):.1f}"})
# Host side of the chain: per-packet server cost (parse + handle, which includes the estimator solve) from the
# replay benchmark through the real packet path (campaign host_bench.log, 4 markers, worst of pixel/point mode),
# times 4 packets per slot. A complete slot is solved on its last packet, so pose = last packet + this.
HB = [l.split("|") for l in (CAMP / "host_bench.log").read_text().splitlines() if l.startswith(("pixel |", "point |"))]
HB = [r for r in HB if int(r[1]) == 4]
assert len(HB) == 2
SOLVE_MS = max(float(r[3]) + float(r[4]) for r in HB) * 4 / 1e3
N.update({"EeSolveMs": f"{SOLVE_MS:.1f}",
          "EePoseMed": f"{(LAT['capture_to_last_camera_at_host_us']['median'] / 1e3 + SOLVE_MS):.1f}",
          "EePosePn": f"{(LAT['capture_to_last_camera_at_host_us']['p95'] / 1e3 + SOLVE_MS):.1f}",
          "EeRttLo": f"{max(v['p5'] for v in LATF['icmp_rtt_ms'].values()):.0f}"})
assert SOLVE_MS < .5 and float(N["EePoseMed"]) >= float(N["EeHostMed"])
assert [N[k] for k in ("EeSlotMs", "EeReadyMs", "EeProcUs", "EeSendMs", "EeHostMed", "EeHostPn")] == \
    ["28.6", "18.2", "360", "18.6", "29.1", "60.6"], {k: N[k] for k in N if k.startswith("Ee")}
assert LAT["fps"] == int(N["AbFps"]) and float(N["EeReadyMs"]) < float(N["EeSendMs"]) < float(N["EeHostMed"]) < float(N["EeHostPn"])
# Rate ceiling of the shipped mode with no Studio cap (dataset fps-ceiling/nocap, fps_ceiling.py, 60 s per rate, 4 cameras)
CEIL = {r["fps"]: r for r in json.load((CAMP / "fps-ceiling/nocap/fps_ceiling.stats.json").open())["rates"]}
N.update({"CeilFps": "40", "CeilFull": f"{100 * CEIL[40]['full_slot_yield']:.1f}",
          "CeilCamLo": f"{math.floor(10 * min(CEIL[40]['per_camera_fps'].values())) / 10:.1f}",   # floored: a ">=" bound
          "CeilOverFps": "44", "CeilOverFull": f"{100 * CEIL[44]['full_slot_yield']:.0f}"})
assert float(N["CeilFull"]) > 95 > float(N["CeilOverFull"]) and float(N["CeilCamLo"]) > 39, {k: N[k] for k in N if k.startswith("Ceil")}
(OUT / "numbers.tex").write_text("".join(f"\\newcommand{{\\{k}}}{{{v}}}\n" for k, v in N.items()))
cap = [  # Table 3 rows: quantity | tracking SW | tracking HW (FTM-timed tracking pair only; the AP-TSF-timed 40-min runs are not in it)
    (r"Delay (per-camera medians)", f"{N['SwDelayLo']}--{N['SwDelayHi']}", f"{N['HwDelayLo']}--{N['HwDelayHi']}"),
    (r"Spread, med./p95", f"{N['SwSpMed']}/{N['SwSpPn']}", f"{N['HwSpMed']}/{N['HwSpPn']}"),
    (r"Delay-removed spread, med./p95", f"{N['SwJitMed']}/{N['SwJitPn']}", f"{N['HwJitMed']}/{N['HwJitPn']}"),
    (r"Full slots (\%)", N["SwYield"], N["HwYield"]),
]
(OUT / "table_capture.tex").write_text("\n".join(" & ".join(r) + r" \\" for r in cap) + "\n")
json.dump({"offset_stats": stats, "capture_numbers": N, "follower_means": {f"{k[0]}|{k[1]}": v for k, v in fmeans.items()},
           "three_cornered_hat": TCH, "nosync_drift_us_per_s": drift, "short_mcpwm_max": max(short)}, (OUT / "stats.json").open("w"), indent=1)
for k, s in stats.items():
    print(f"{k:45s} n={s['n']:4d} mean={s['mean']:+11.3f} sd={s['sd']:10.3f} p95={s['p95']:10.3f} p99={s['p99']:10.3f} max={s['max']:10.3f}")
print("figures ->", OUT)


def layout_clashes(fig):
    """Text/text overlaps and lines passing through text, in display pixels (assert-checked below)."""
    fig.canvas.draw(); r = fig.canvas.get_renderer()
    from matplotlib.transforms import Bbox
    T = [(t.get_text(), t.get_window_extent(r).shrunk(.97, .8)) for ax in fig.axes for t in ax.texts if t.get_text().strip()]
    bad = [(a, b) for n, (a, A) in enumerate(T) for b, B in T[n + 1:] if A.overlaps(B)]
    for ax in fig.axes:
        for ln in ax.lines:
            P = ln.get_transform().transform(ln.get_xydata())
            for t, B in T:
                if any(B.count_contains(P) for _ in [0]) or any(
                        Bbox([[min(p[0], q[0]), min(p[1], q[1])], [max(p[0], q[0]) + .01, max(p[1], q[1]) + .01]]).overlaps(B)
                        and _seg_hits(p, q, B) for p, q in zip(P, P[1:])):
                    bad.append((t, "line"))
        for tx in ax.texts:                                                    # annotation arrows vs other text
            ap = getattr(tx, "arrow_patch", None)
            if ap is not None:
                AB = ap.get_window_extent(r)
                bad += [(t, "arrow") for t, B in T if t != tx.get_text() and AB.overlaps(B)]
        for pa in ax.patches:
            PB = pa.get_window_extent(r)
            for t, B in T:
                if PB.overlaps(B) and not (PB.x0 <= B.x0 and B.x1 <= PB.x1 and PB.y0 <= B.y0 and B.y1 <= PB.y1):
                    bad.append((t, "patch edge"))
    return bad


def _seg_hits(p, q, B, n=40):
    return any(B.contains(p[0] + (q[0] - p[0]) * k / n, p[1] + (q[1] - p[1]) * k / n) for k in range(n + 1))


# ---- Fig 1: system and clock-layer test setup ----
# (c) uses the hardware-path A/B run medians (N["Ee*"], N["HwDelay*"], N["RowSpanMs"]).
D = "\u2013"
fig, a = plt.subplots(figsize=(3.5, 0.98)); a.set_axis_off()
a.set_xlim(0, 10); a.set_ylim(0, 6.4)
def box(ax, x, y, w, h, t, fc="white", fs=7.5):
    ax.add_patch(plt.Rectangle((x, y), w, h, fc=fc, ec="k", lw=.7))
    ax.text(x + w / 2, y + h / 2, t, ha="center", va="center", fontsize=fs, linespacing=1.0)
def arrow(ax, p, q, **k):
    ax.annotate("", q, p, arrowprops=dict(arrowstyle="->", lw=.7, shrinkA=0, shrinkB=0, **k))
HOSTW = 6.0
box(a, (10 - HOSTW) / 2, 5.35, HOSTW, .95, "host: hotspot AP, triangulation", "0.93")   # centred on the column
a.plot([.15, 9.85], [4.2, 4.2], ls="--", lw=.9, color="#1f77b4")                        # Wi-Fi link, full width
a.text(.15, 4.38, "Wi-Fi: centroids (UDP), FTM", ha="left", va="bottom", fontsize=FSMIN, color="#1f77b4")
arrow(a, (5, 4.2), (5, 5.35), color="0.4")                                              # link to host, on its centre
for i, x in enumerate([.15, 2.65, 5.15, 7.65]):
    ref = i == 2
    box(a, x, 2.1, 2.2, 1.6, f"node {i}\n" + ("responder" if ref else "initiator"), "#dbe9f6" if ref else "white")
    a.plot([x + 1.1] * 2, [3.7, 4.2], color="#1f77b4", lw=.9)
    arrow(a, (x + 1.1, 2.1), (x + 1.1, 1.05), color="k")
box(a, .15, .1, 9.7, .95, "logic analyzer, 16 MS/s (GPIO41 of each node)", "0.93")      # GPIO pin named once
fig.tight_layout(pad=.1)
assert not layout_clashes(_fig_arm), layout_clashes(_fig_arm)
_r = _fig_arm.canvas.get_renderer(); _ax2 = _fig_arm.axes[1]
_pts = _ax2.transData.transform(np.vstack([_cpts, _leds[:, :2]]))          # LED dots, display px
_pad = 3.0 / 72 * _fig_arm.dpi                                             # dot radius + gap (3 pt)
for _t in _ax2.texts:                                                      # labels clear of every LED dot, and inside the figure
    _B = _t.get_window_extent(_r)
    assert not any(_B.x0 - _pad < x < _B.x1 + _pad and _B.y0 - _pad < y < _B.y1 + _pad for x, y in _pts), (_t.get_text(), "dot", _B, [p for p in _pts if _B.x0 - _pad < p[0] < _B.x1 + _pad and _B.y0 - _pad < p[1] < _B.y1 + _pad], _pad)
    assert _B.y1 <= _fig_arm.bbox.y1 and _B.x1 <= _fig_arm.bbox.x1, (_t.get_text(), "edge", _B, _fig_arm.bbox)
plt.close(_fig_arm)
clash = layout_clashes(fig)
assert not clash, f"Fig. 1 layout clashes: {clash}"
fig.savefig(OUT / "system.pdf"); plt.close(fig)

# ---- Fig 2: end-to-end flow of one slot: node chain on top, host chain below ----
fig, b = plt.subplots(figsize=(3.5, 0.98)); b.set_axis_off()
b.set_xlim(0, 10); b.set_ylim(0, 5.55)
NODE, HOST, FS = "#e6f2e6", "0.93", FSMIN
W, G, H = 1.66, .30, 1.30                                                       # one box size for both rows
XS = [.25 + k * (W + G) for k in range(5)]
def enclose(x0, y0, x1, y1, label, color, inside=True):                         # dashed group box
    b.add_patch(plt.Rectangle((x0, y0), x1 - x0, y1 - y0, fc="none", ec=color, lw=.6, ls=(0, (3, 2))))
    if inside:                                                                  # label in the box's own top-right
        b.text(x1 - .30, y1 - .22, label, fontsize=FS, ha="right", va="top", color=color)
    else:                                                                       # label outside, right of the corner
        b.text(x1 + .18, y1, label, fontsize=FS, ha="left", va="top", color=color)
YN, YH = 3.38, .25                                                              # node row, host row baselines
enclose(.05, 3.18, 9.95, 5.50, "camera node (\u00d74), every slot", "#2d6a2d")
enclose(.05, .05, 4.07, 1.75, "host", "0.35", inside=False)                     # label outside: the row has free width
for k, (t, fc) in enumerate([("FTM clock\nmodel (1 Hz)", "white"), ("timer at\nslot $kP$", NODE), ("FREX\ntrigger", NODE),
                             ("readout,\nstamp $t_0$", NODE), ("threshold,\ncentroids", NODE)]):
    box(b, XS[k], YN, W, H, t, fc, FS)
    if k:
        arrow(b, (XS[k] - G, YN + H / 2), (XS[k], YN + H / 2))
for k, t in enumerate(["group by\n$t_0+y\\,t_\\mathrm{row}$", "triangulate,\nfilter 3-D"]):
    box(b, XS[k], YH, W, H, t, HOST, FS)
    if k:
        arrow(b, (XS[k] - G, YH + H / 2), (XS[k], YH + H / 2))
b.plot([XS[4] + W / 2, XS[4] + W / 2, XS[0] + W / 2], [YN, 2.55, 2.55], color="k", lw=.7)   # node row -> host row
arrow(b, (XS[0] + W / 2, 2.55), (XS[0] + W / 2, YH + H))
b.text(XS[4] + W / 2 - .22, 2.44, "UDP over Wi-Fi", fontsize=FS, ha="right", va="top")      # clear of both enclosures
arrow(b, (XS[1] + W, YH + H / 2), (XS[1] + W + .75, YH + H / 2))                # output of the chain
b.text(XS[1] + W + .9, YH + H / 2, "3-D track", fontsize=FS, ha="left", va="center")
fig.tight_layout(pad=.1)
clash = layout_clashes(fig)
assert not clash, f"Fig. 2 (flow) layout clashes: {clash}"
fig.savefig(OUT / "flow.pdf"); plt.close(fig)

# ---- Fig 4: end-to-end latency, consecutive slots as a pipeline (shipped mode, medians, p95 whisker) ----
# One row per slot, one bar per row split into stages, each row shifted by the slot period, so the reader sees
# slot n+1 being read out while slot n is still on Wi-Fi.
rdy, snd, hst, hp95 = (float(N[k]) for k in ("EeReadyMs", "EeSendMs", "EeHostMed", "EeHostPn"))
pose, P = float(N["EePoseMed"]), float(N["EeSlotMs"])
STAGES = [("readout (node)", 0, rdy, "#1f77b4"), ("detect, send", rdy, snd, "#ff7f0e"),
          ("Wi-Fi, last camera", snd, hst, "#9467bd"), ("solve (host)", hst, pose, "#2ca02c")]
assert STAGES[0][1] == 0 and all(a_[2] == b_[1] for a_, b_ in zip(STAGES, STAGES[1:])) and STAGES[-1][2] == pose
NS = 4
fig, c = plt.subplots(figsize=(3.5, 1.45))
for n in range(NS):
    t = n * P
    for lab, t0, t1, col in STAGES:
        c.barh(n, max(t1 - t0, .9), left=t + t0, height=.6, color=col, ec="none", label=lab if n == 0 else None)
    c.plot([t + hst, t + hp95], [n, n], color="0.25", lw=.6)
    c.plot([t + hp95] * 2, [n - .18, n + .18], color="0.25", lw=.6)

c.set_yticks(range(NS), [f"slot {n + 1}" for n in range(NS)], fontsize=FSMIN); c.set_ylim(NS - .4, -.6)
c.set_xlim(0, (NS - 1) * P + hp95 + 6); c.grid(visible=False)
for k in range(NS):
    c.axvline(k * P, ls="--", lw=.5, color="0.55", zorder=0)
c.set_xticks(np.arange(0, (NS - 1) * P + hp95 + 2, 20))
c.set_xlabel(f"time (ms), dashed = frame start every {P:.1f} ms", fontsize=FSMIN)
c.tick_params(labelsize=FSMIN, length=2)
fig.legend(loc="upper center", ncol=4, fontsize=FSMIN, frameon=False, handlelength=1, columnspacing=.8, borderaxespad=.1)
fig.tight_layout(pad=.3, rect=(0, 0, 1, .86))
clash = layout_clashes(fig)
assert not clash, f"Fig. 4 (latency) layout clashes: {clash}"
fig.savefig(OUT / "timeline.pdf"); plt.close(fig)
