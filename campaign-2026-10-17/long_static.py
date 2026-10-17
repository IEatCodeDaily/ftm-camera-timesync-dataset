"""Long static run: 10-min bins of the parked Dobot marker (4 LEDs) from dynamic_n.py output.
Reports per-bin centroid and LED-distance means; drift = range across bins.
Usage: python3 long_static.py <dynamic_n out.json> <out.json>
"""
import json, sys
import numpy as np

rows = json.load(open(sys.argv[1]))["rows"]["0.0"]
t = np.array([r["host_ns"] for r in rows]) / 1e9; t -= t[0]
C = np.array([r["pos"] for r in rows]); D = np.array([r["d_mm"] for r in rows])
bins = []
for b in range(int(t[-1] // 600) + 1):
    m = (t >= b * 600) & (t < (b + 1) * 600)
    if m.sum() < 100: continue
    bins.append({"t_min": b * 10, "n": int(m.sum()), "centroid_mm": C[m].mean(0).tolist(), "centroid_sd_mm": C[m].std(0).tolist(),
                 "d_mean_mm": D[m].mean(0).tolist()})
Cb = np.array([b["centroid_mm"] for b in bins]); Db = np.array([b["d_mean_mm"] for b in bins])
out = {"duration_min": float(t[-1] / 60), "slots": len(rows), "bins_10min": bins,
       "centroid_range_mm": np.ptp(Cb, 0).tolist(), "d_range_max_mm": float(np.ptp(Db, 0).max()),
       "centroid_sd_mm_all": C.std(0).tolist(), "d_sd_mm_all": D.std(0).tolist()}
assert len(bins) >= 5, len(bins)
json.dump(out, open(sys.argv[2], "w"), indent=1)
print(json.dumps({k: v for k, v in out.items() if k != "bins_10min"}, indent=1))
