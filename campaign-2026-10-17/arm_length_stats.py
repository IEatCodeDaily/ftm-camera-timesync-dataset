"""Length error vs the Dobot reference (Eichelberger 2016 style). For every pair of reached poses:
e = |tracked_i - tracked_j| - |arm_i - arm_j|. Arm distances come from the arm's encoders (robot frame),
tracked distances from the tracker (world frame): no registration or scale fit enters.
Usage: python arm_length_stats.py arm-length.json out.json"""
import json, sys, math, itertools, statistics as st
d = json.load(open(sys.argv[1]))
# Group by the pose the arm REPORTS (commands outside the workspace snap to a reachable point;
# several snapped to the same one). One reference point per distinct reached pose, tracked = median.
G = {}
for r in d["rows"]:
    if "pose_xyz" in r: G.setdefault(tuple(round(x) for x in r["pose_xyz"]), []).append(r)
R = [{"pose_xyz": [st.median(r["pose_xyz"][k] for r in v) for k in range(3)],
      "ir_world": [st.median(r["ir_world"][k] for r in v) for k in range(3)],
      "ir_sd": [max(max(r["ir_sd"]) for r in v)] * 3, "visits": len(v)} for v in G.values()]
e, L = [], []
for a, b in itertools.combinations(R, 2):
    l = math.dist(a["pose_xyz"], b["pose_xyz"]); L.append(l); e.append(math.dist(a["ir_world"], b["ir_world"]) - l)
q = lambda v, p: sorted(v)[min(len(v) - 1, int(p * len(v)))]
n = len(e); mean = sum(e) / n
slope = sum((l - st.mean(L)) * (x - mean) for l, x in zip(L, e)) / sum((l - st.mean(L)) ** 2 for l in L)
long_ = [x for x, l in zip(e, L) if l >= 200]
out = {"poses": len(R), "attempted": len(d["rows"]), "pairs": n, "len_min_mm": min(L), "len_max_mm": max(L),
       "rms_mm": math.sqrt(sum(x * x for x in e) / n), "mean_mm": mean, "abs_median_mm": st.median(abs(x) for x in e),
       "abs_p95_mm": q([abs(x) for x in e], 0.95), "abs_max_mm": max(abs(x) for x in e),
       "rel_rms_pct": 100 * math.sqrt(sum((x / l) ** 2 for x, l in zip(e, L)) / n), "scale_err_pct": 100 * slope,
       "long_pairs": len(long_), "long_rms_mm": math.sqrt(sum(x * x for x in long_) / max(1, len(long_))),
       "static_sd_median_mm": st.median(max(r["ir_sd"]) for r in R),
       "arm_box_mm": [[min(r["pose_xyz"][k] for r in R), max(r["pose_xyz"][k] for r in R)] for k in range(3)]}
json.dump(out, open(sys.argv[2], "w"), indent=1); print(json.dumps(out, indent=1))
