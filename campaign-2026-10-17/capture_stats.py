"""Capture-layer statistics of the 2026-10-17 paired A/B run (ab/tracking-{hw,sw}.mcap).

Same per-recording analysis as ../tracking/analyze_tracking.py (slot grid, delay, delay-removed
spread, row-readout offsets). Firmware 842af0eb, VGA, 35 frames/s, vertical subsampling 4: the
packet row period (25 750 ns) matches the HREF-measured output row (rowtiming/, 25.75 us x 480 rows).
Run: ~/.local/paperenv/.venv/bin/python capture_stats.py  -> capture.stats.json
"""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tracking"))
import analyze_tracking as at

out = {}
for arm in ("hw", "sw"):
    r = at.per_recording(os.path.join(HERE, "ab", f"tracking-{arm}.mcap"))
    r.pop("_slot"); r.pop("_fps")
    out[arm] = r
h = out["hw"]
# the recording must be the shipped setting whose row period was checked against HREF
assert h["row_period_ns"] == 25750 and h["resolution"] == [640, 480] and h["inferred_fps"] == 35
href = open(os.path.join(HERE, "rowtiming", "href-vskip4-192.168.137.172.txt")).read()
assert "@25.75]" in href and "+480@" in href
# row offset dwarfs the hardware capture spread (the claim the paper prints)
assert h["C_cross_camera_row_time_diff_us"]["median"] / 2 > 10 * h["B_capture_spread_after_fixed_offset_us"]["p95"]
json.dump(out, open(os.path.join(HERE, "capture.stats.json"), "w"), indent=1, default=float)
print(json.dumps({a: {"row_med_us": out[a]["C_marker_row_time_us"]["median"], "xrow_med_us": out[a]["C_cross_camera_row_time_diff_us"]["median"],
                      "jit_med_us": out[a]["B_capture_spread_after_fixed_offset_us"]["median"]} for a in out}, indent=1))
