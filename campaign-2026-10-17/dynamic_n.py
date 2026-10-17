"""Dynamic 3-D check, 4-LED Dobot marker (campaign 2026-10-17). Same method as
../arm-reference/dynamic_analyse.py (row-slope interpolation to one instant per
slot, all cameras triangulated together, distance error = moving - static of
the same recording), generalised to an N-LED body among stray blobs:
per slot, each camera's blobs are matched to the body LEDs by reprojecting the
body points triangulated from the two cameras whose blob pairing best fits the
body's own inter-LED distances. Rows: every slot where all LEDs triangulate
from >=2 cameras with max reprojection residual < RESID_PX.

Usage: python3 dynamic_n.py <mcap> <out.json> <body-name> seg-*.json
"""
import itertools, json, math, os, sys
import numpy as np
from mcap.reader import make_reader

HERE = os.path.dirname(os.path.abspath(__file__))
RESID_PX = 3.0
sys.path.insert(0, os.path.join(HERE, "..", "arm-reference"))
import dynamic_analyse as da  # rig(), tri()
da.RIG = os.path.join(HERE, "rig")


def main():
    mcap, out, body = sys.argv[1], sys.argv[2], sys.argv[3]
    B = np.array(next(b for b in json.load(open(os.path.join(HERE, "rig", "rigid_bodies.json"))) if b["name"] == body)["points_mm"])
    N = len(B); Dref = {(i, j): np.linalg.norm(B[i] - B[j]) for i, j in itertools.combinations(range(N), 2)}
    segs = []
    for f in sys.argv[4:]:
        s = json.load(open(f)); segs += [dict(x, label=s["label"], speed_pct=s["speed_pct"]) for x in s["segments"]]
    t_lo, t_hi = min(x["t0_ns"] for x in segs) - 5e9, max(x["t1_ns"] for x in segs) + 5e9
    rows = [json.loads(m.data) for _, _, m in make_reader(open(mcap, "rb")).iter_messages(topics=["/mocap/centroids"])]
    rows = [r for r in rows if t_lo <= r["host_time_ns"] <= t_hi]
    P = da.rig()
    rp = {r["row_period_ns"] for r in rows}; assert len(rp) == 1, rp; t_row = rp.pop() / 1000.0
    period = float(np.median(np.diff(np.sort([r["capture_us"] for r in rows if r["camera_id"] == 0]))))
    slot = {}
    for r in rows:
        if r["camera_id"] in P and len(r["markers"]) >= N:
            slot.setdefault(round(r["capture_us"] / period), {})[r["camera_id"]] = r
    host_of = {k: min(r["host_time_ns"] for r in s.values()) for k, s in slot.items()}

    def fit_pair(a, b, ma, mb):
        """Best assignment of body LEDs to blobs in cameras a,b: N 3-D points whose distances match the body."""
        best = None
        cand = []
        for i, pa in enumerate(ma):
            for j, pb in enumerate(mb):
                X, r = da.tri(P, [(a, pa), (b, pb)])
                if r < RESID_PX: cand.append((i, j, X))
        for combo in itertools.permutations(range(len(cand)), N) if len(cand) <= 9 else ():
            pts = [cand[c] for c in combo]
            if len({p[0] for p in pts}) < N or len({p[1] for p in pts}) < N: continue
            e = max(abs(np.linalg.norm(pts[u][2] - pts[v][2]) - Dref[(u, v)]) for u, v in Dref)
            if best is None or e < best[0]: best = (e, [p[2] for p in pts])
        return best

    lab = {}
    for k, s in slot.items():
        cams = sorted(s)
        if len(cams) < 2: continue
        found = None
        for a, b in itertools.combinations(cams, 2):
            f = fit_pair(a, b, [(m["x"], m["y"]) for m in s[a]["markers"]], [(m["x"], m["y"]) for m in s[b]["markers"]])
            if f and f[0] < 8.0 and (found is None or f[0] < found[0]): found = f
        if not found: continue
        per = {}
        for c in cams:
            pts = [(m["x"], m["y"]) for m in s[c]["markers"]]; assign = []
            for Xi in found[1]:
                q = P[c] @ np.append(Xi, 1); q = q[:2] / q[2]
                j = min(range(len(pts)), key=lambda j: math.hypot(pts[j][0] - q[0], pts[j][1] - q[1]))
                assign.append(j if math.hypot(pts[j][0] - q[0], pts[j][1] - q[1]) < 6 else None)
            if None not in assign and len(set(assign)) == N:
                per[c] = ([pts[j] for j in assign], s[c]["capture_us"])
        if len(per) >= 2: lab[k] = per
    ks = sorted(lab)

    def at(k, c, led, slope, t_star):
        samp = []
        for kk in (k - 1, k, k + 1):
            if kk in lab and c in lab[kk]:
                pts, t0 = lab[kk][c]; x, y = pts[led]; samp.append((t0 + slope * y * t_row, x, y))
        samp.sort()
        for (ta, xa, ya), (tb, xb, yb) in zip(samp, samp[1:]):
            if ta <= t_star <= tb and tb > ta:
                w = (t_star - ta) / (tb - ta); return (xa + w * (xb - xa), ya + w * (yb - ya))
        return None

    pairs = list(itertools.combinations(range(N), 2)); res = {}
    for slope in (0.0, 0.5, 1.0):
        recs = []
        for k in ks:
            t_star = k * period + 6000.0
            Xs, rmax, n = [], 0.0, 0
            for led in range(N):
                obs = [(c, p) for c in lab[k] if (p := at(k, c, led, slope, t_star)) is not None]
                if len(obs) < 2: break
                X, r = da.tri(P, obs); Xs.append(X); rmax = max(rmax, r); n = len(obs)
            if len(Xs) == N and rmax < RESID_PX: recs.append((k, Xs, rmax, n))
        cen = {k: np.mean(Xs, 0) for k, Xs, _, _ in recs}
        out_rows = []
        for k, Xs, rmax, n in recs:
            if k - 1 in cen and k + 1 in cen:
                vv = (cen[k + 1] - cen[k - 1]) / (2 * period / 1e6)
                out_rows.append({"k": k, "host_ns": host_of[k], "speed_mm_s": float(np.linalg.norm(vv)), "vel": vv.tolist(),
                                 "pos": cen[k].tolist(), "d_mm": [float(np.linalg.norm(Xs[i] - Xs[j])) for i, j in pairs], "resid_px": rmax, "n_cams": n})
        res[str(slope)] = out_rows
    st = np.array([r["d_mm"] for r in res["0.0"] if r["speed_mm_s"] < 2.0]); ref = np.median(st, 0)
    summary = {"body": body, "pairs": pairs, "t_row_us": t_row, "period_us": period, "slots": len(slot), "slots_labelled": len(ks),
               "static_n": len(st), "static_d_mm": ref.tolist(), "static_d_sd_mm": st.std(0, ddof=1).tolist(),
               "body_d_mm": [float(Dref[p]) for p in pairs], "bins": {}}
    for s, rr in res.items():
        b_out = []
        for lo, hi in [(2, 50), (50, 100), (100, 150), (150, 200), (200, 300), (300, 1000)]:
            e = np.array([np.array(r["d_mm"]) - ref for r in rr if lo <= r["speed_mm_s"] < hi])
            if len(e) < 10: continue
            ae = np.abs(e).max(1)
            b_out.append({"speed_lo": lo, "speed_hi": hi, "n": len(e), "rms_mm": float(np.sqrt((e ** 2).mean())),
                          "median_abs_max_mm": float(np.median(ae)), "p95_abs_max_mm": float(np.percentile(ae, 95))})
        summary["bins"][s] = b_out
    summary["max_speed_mm_s"] = float(max(r["speed_mm_s"] for r in res["0.0"]))
    json.dump({"summary": summary, "rows": res}, open(out, "w"))
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
