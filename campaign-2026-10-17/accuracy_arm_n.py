"""Independent-scale length test from the robot-arm reference (arm-reference.json).

The arm reports its tool point from its own joint encoders/stepper counts, so the distance
between two reached poses is a reference length that never passed through the tracker or
its registration. Compare it with the tracked distance between the same two poses.
Visits are grouped by the pose the arm actually reported (three z=85 commands were out of
reach and stopped elsewhere, so commanded != reached there).

Usage: python3 accuracy_arm.py <arm-reference.json> <out.json>
"""
import sys, json, math, itertools
import numpy as np

d = json.load(open(sys.argv[1]))
by = {}
for r in d["rows"]:
    by.setdefault(tuple(round(x, 1) for x in r["pose_xyz"]), []).append(np.array(r["ir_world"]))
P = [(np.mean(v, 0), np.array(k)) for k, v in by.items()]
e, L = [], []
for (a, pa), (b, pb) in itertools.combinations(P, 2):
    L.append(np.linalg.norm(pa - pb)); e.append(np.linalg.norm(a - b) - L[-1])
e, L = np.array(e), np.array(L)
print("poses", len(P), "pairs", len(e))
out = {"poses": len(P), "pairs": len(e), "len_min_mm": float(L.min()), "len_max_mm": float(L.max()),
       "err_mean_mm": float(e.mean()), "err_sd_mm": float(e.std(ddof=1)), "err_rms_mm": float(math.sqrt((e ** 2).mean())),
       "abs_median_mm": float(np.median(abs(e))), "abs_p95_mm": float(np.percentile(abs(e), 95)), "abs_max_mm": float(abs(e).max()),
       "rel_rms_pct": float(100 * math.sqrt(((e / L) ** 2).mean())), "scale": float(np.polyfit(L, L + e, 1)[0]),
       "held_out_err_median_mm": d["summary"]["err_median_mm"], "held_out_err_rms_mm": d["summary"]["err_rms_mm"],
       "held_out_err_max_mm": d["summary"]["err_max_mm"], "held_out_n": d["summary"]["n_ok"]}
json.dump(out, open(sys.argv[2], "w"), indent=1)
print(json.dumps(out, indent=1))
