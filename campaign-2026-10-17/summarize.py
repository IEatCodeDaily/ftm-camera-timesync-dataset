"""Campaign 2026-10-17 summary: every number in CAMPAIGN_REPORT.md comes from here.
Run: ~/.local/paperenv/.venv/bin/python summarize.py  -> campaign.stats.json (+ prints)
"""
import glob, itertools, json, math, os, statistics as st
H = os.path.dirname(os.path.abspath(__file__)); J = lambda *p: os.path.join(H, *p)
ld = lambda p: json.load(open(J(p)))
out = {}

# A: firmware ladder (thesis ablation harness, 60 s each, operator detector knobs)
abl = {}
for f in sorted(glob.glob(os.path.expanduser("~/thesis/book/measure/runs-fw2610/*.json"))):
    d = json.load(open(f)); a = d.get("analysis")
    if not a: continue
    q = lambda k: (a[k] or {}).get("median"), 
    abl[d["name"]] = {"fps": d["config"]["fps"], "vskip": d["config"]["vskip"], "lowlat": d["config"]["lowlat"],
                      "full_slots": a["full_slots"], "full_pct": 100 * a["full_slots"] / (d["dur"] * d["config"]["fps"]),
                      "send_med_ms": a["send_after_capture_us"]["median"] / 1e3,
                      "e2e_med_ms": (a["capture_to_last_camera_at_host_us"] or {}).get("median", float("nan")) / 1e3,
                      "e2e_p95_ms": (a["capture_to_last_camera_at_host_us"] or {}).get("p95", float("nan")) / 1e3,
                      "markers_per_packet": a["markers_per_packet"]}
out["ablation"] = abl

# B: hw vs sw capture timing (90 s each)
ab = ld("ab/tracking_ab.stats.json")
out["capture_ab"] = {k: {"full_yield": v["full_yield"], "spread_med_us": v["spread_us"]["median"],
                         "jitter_med_us": v["jitter_fixed_removed_us"]["median"], "jitter_p99_us": v["jitter_fixed_removed_us"]["p99"],
                         "per_camera_offset_us": v["per_camera_offset_from_slot_us"]} for k, v in ab.items()}

# C: latency probe (shipped config, 90 s per rate)
lat = ld("latency/fast/latency-lowlat-vskip4.stats.json")
out["latency"] = {r["fps"]: {"send_med_ms": r["send_after_capture_us"]["median"] / 1e3,
                             "deliv_excess_med_ms": r["delivery_excess_us"]["median"] / 1e3, "deliv_excess_p95_ms": r["delivery_excess_us"]["p95"] / 1e3,
                             "e2e_med_ms": r["capture_to_last_camera_at_host_us"]["median"] / 1e3, "e2e_p95_ms": r["capture_to_last_camera_at_host_us"]["p95"] / 1e3,
                             "full_slots": r["capture_to_last_camera_at_host_us"]["n"]} for r in lat["rates"]}

# D: arm static reference (18 held-out poses x 2, speed 15 %)
ar = ld("arm-reference.json"); s = ar["summary"]; ok = [r for r in ar["rows"] if "err" in r]
pe = [math.dist(a["ir_world"], b["ir_world"]) - math.dist(a["reported_world"], b["reported_world"])
      for a, b in itertools.combinations(ok, 2) if a["pose_xyz"] != b["pose_xyz"]]
out["arm_static"] = {"n_ok": s["n_ok"], "n_fail": s["n_fail"], "abs_err_median_mm": s["err_median_mm"], "abs_err_rms_mm": s["err_rms_mm"],
                     "per_axis_rms_mm": s["per_axis_rms_mm"], "ir_sd_median_mm": s["static_ir_sd_median_mm"],
                     "repeat_dist_median_mm": s["repeat_ir_dist_median_mm"], "arm_calibration_rms_mm": s["calibration"]["rms_mm"],
                     "pair_length_n": len(pe), "pair_length_rms_mm": math.sqrt(sum(x * x for x in pe) / len(pe)),
                     "pair_length_median_abs_mm": st.median(abs(x) for x in pe),
                     "abs_err_rep0_median_mm": st.median(r["err_norm"] for r in ok if r["rep"] == 0),
                     "abs_err_rep1_median_mm": st.median(r["err_norm"] for r in ok if r["rep"] == 1)}

# E: dynamic (4-LED body rigidity under motion) + direction-paired
dy = ld("dynamic.json")["summary"]; pa = ld("paired.json")
out["dynamic"] = {"max_speed_mm_s": dy["max_speed_mm_s"], "slots_labelled": dy["slots_labelled"], "slots": dy["slots"],
                  "static_d_sd_mm": dy["static_d_sd_mm"],
                  "bins": {sl: [{k: b[k] for k in ("speed_lo", "speed_hi", "n", "rms_mm", "median_abs_max_mm", "p95_abs_max_mm")} for b in bb] for sl, bb in dy["bins"].items()},
                  "paired_half_diff_median_mm": {sl: {k: v["half_diff_all_pairs_median_mm"] for k, v in d.items()} for sl, d in pa.items()}}

# F: 40-min FREX wrap run (nodes-only)
w = json.loads(open(J("wrap.analysis.txt")).read()[open(J("wrap.analysis.txt")).read().find("{"):])
out["wrap"] = {k: w[k] for k in ("duration_s", "frames", "full_slot_yield", "full_yield_of_logged", "spread_us", "jitter_spread_us", "per_node_median_delay_us",
                                 "per_node_1min_median_range_us", "ftm_wraps_crossed", "sync_lost_checks", "logger_errors")}
out["wrap"]["node_tx_sent"] = {r["addr"]: int(r["reply"].split("tx_sent=")[1].split()[0]) for r in map(json.loads, open(J("wrap-40min.jsonl"))) if r["kind"] == "stats" and r["cmd"] == "sync"}

out["exposure_gain"] = open(J("tune-before.txt")).read().strip().splitlines()
json.dump(out, open(J("campaign.stats.json"), "w"), indent=1)

# sanity asserts on the claims the report makes
assert all(v["full_pct"] > 99 for k, v in abl.items() if k != "F1_vskip4")
assert out["capture_ab"]["hw"]["jitter_med_us"] < out["capture_ab"]["sw"]["jitter_med_us"]
assert max(b["rms_mm"] for bb in out["dynamic"]["bins"].values() for b in bb) < 1.0
assert out["arm_static"]["n_fail"] == 0
print(json.dumps(out, indent=1, default=str)[:6000])
