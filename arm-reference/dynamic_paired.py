"""Direction-paired dynamic test (offline, on dynamic_analyse.py output).

Each arm sweep passes the same positions forwards and backwards at the same
speed. Position-dependent calibration bias is identical on both passes and
cancels in (forward - backward); a timing error shears the LED triangle along
the motion, so it flips sign with direction and survives. Per sweep segment
pair, slots are binned by position along the sweep (10 mm); the statistic is
the median over bins of |d_fwd - d_bwd| / 2 per LED pair, per speed setting,
for row slopes 0 / 0.5 / 1.

Usage: python3 dynamic_paired.py <dyn.json> <out.json> seg-*.json
"""
import json, sys
import numpy as np

BIN_MM = 10.0
CLOCK_TOL_NS = 300_000_000  # host (Windows) vs script (WSL) wall clock; segments are >1 s with 0.5 s gaps


def main():
    dyn = json.load(open(sys.argv[1])); out = sys.argv[2]
    segs = []
    for f in sys.argv[3:]:
        s = json.load(open(f)); segs += [dict(x, label=s["label"], speed_pct=s["speed_pct"]) for x in s["segments"]]
    res = {}
    for slope, rows in dyn["rows"].items():
        rows = [r for r in rows if r["speed_mm_s"] < 600]  # > arm max: mislabel jumps
        per = {}
        for g in segs:
            sel = [r for r in rows if g["t0_ns"] + CLOCK_TOL_NS <= r["host_ns"] <= g["t1_ns"] - CLOCK_TOL_NS]
            if len(sel) < 5:
                continue
            P = np.array([r["pos"] for r in sel])
            per.setdefault((g["label"], g["sweep"]), []).append((P, np.array([r["d_mm"] for r in sel]), np.array([r["speed_mm_s"] for r in sel])))
        stats = {}
        for (label, sweep), passes in per.items():
            allP = np.vstack([p[0] for p in passes])
            axis = np.linalg.svd(allP - allP.mean(0))[2][0]
            # passes alternate direction (b then a); split by sign of the motion along axis
            fwd, bwd = [], []
            for P, D, V in passes:
                u = (P - allP.mean(0)) @ axis
                (fwd if u[-1] > u[0] else bwd).append((u, D, V))
            if not fwd or not bwd:
                continue
            def binned(group):
                u = np.concatenate([g[0] for g in group]); D = np.vstack([g[1] for g in group]); V = np.concatenate([g[2] for g in group])
                b = np.floor(u / BIN_MM).astype(int)
                return {k: (np.median(D[b == k], 0), float(np.median(V[b == k]))) for k in set(b) if (b == k).sum() >= 2}
            F, B = binned(fwd), binned(bwd)
            common = sorted(set(F) & set(B))
            if len(common) < 3:
                continue
            half = np.array([np.abs(F[k][0] - B[k][0]) / 2 for k in common])
            spd = np.array([(F[k][1] + B[k][1]) / 2 for k in common])
            stats[f"{label}/{sweep}"] = {"bins": len(common), "speed_median_mm_s": float(np.median(spd)), "speed_max_mm_s": float(spd.max()),
                                         "half_diff_median_mm": np.median(half, 0).tolist(), "half_diff_all_pairs_median_mm": float(np.median(half))}
        res[slope] = stats
    json.dump(res, open(out, "w"), indent=1)
    for slope, st in res.items():
        print("slope", slope)
        for k, v in sorted(st.items()):
            print(f"  {k:18s} bins {v['bins']:3d} v_med {v['speed_median_mm_s']:6.1f} v_max {v['speed_max_mm_s']:6.1f}  |fwd-bwd|/2 med {v['half_diff_all_pairs_median_mm']:.2f} mm  per pair {[round(x, 2) for x in v['half_diff_median_mm']]}")


if __name__ == "__main__":
    main()
