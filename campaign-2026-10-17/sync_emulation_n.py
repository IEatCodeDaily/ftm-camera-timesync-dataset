"""Time-source emulation on the 2026-10-17 4-LED Dobot sweeps (same method as thesis
measure/sync_emulation.py, generalised to the 4-LED body via dynamic_n.py labelling).
One camera's observation is taken from t + dt (its own image track, linear interpolation);
dt = the worst capture offset each time source showed in the clock record (paper stats.json).
"FTM + row residual" adds half the cross-camera row-time difference measured on THIS data
at the current row period (vskip 4, 25.75 us/row). Emulation on real tracks, not a live run.
Usage: python3 sync_emulation_n.py <out.json> <mcap> <body> seg-*.json
"""
import json, math, sys, os, itertools
import numpy as np
from mcap.reader import make_reader
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dynamic_n as dn
da = dn.da
ST = json.load(open("/mnt/e/Projects/wireless-ir-mocap/paper/figures/stats.json"))["offset_stats"]
SOURCES = [("Live FTM", ST["Live FTM, camera tracking load"]["max"]), ("AP TSF", ST["AP TSF"]["max"]),
           ("FTM + row-readout residual", None), ("NTP-style UDP", ST["NTP-style UDP"]["max"]),
           ("No synchronization", ST["No sync (local MAC)"]["max"])]
SKEW_CAM, SLOPE, RESID_PX = 1, 0.5, 3.0


def main():
    out_p, mcap, body = sys.argv[1], sys.argv[2], sys.argv[3]
    B = np.array(next(b for b in json.load(open(os.path.join(HERE, "rig", "rigid_bodies.json"))) if b["name"] == body)["points_mm"])
    N = len(B); Dref = {(i, j): np.linalg.norm(B[i] - B[j]) for i, j in itertools.combinations(range(N), 2)}
    segs = [g for f in sys.argv[4:] for g in json.load(open(f))["segments"]]
    lo, hi = min(g["t0_ns"] for g in segs) - 5e9, max(g["t1_ns"] for g in segs) + 5e9
    rows = [json.loads(m.data) for _, _, m in make_reader(open(mcap, "rb")).iter_messages(topics=["/mocap/centroids"])]
    rows = [r for r in rows if lo <= r["host_time_ns"] <= hi]
    P = da.rig(); t_row = rows[0]["row_period_ns"] / 1000.0
    period = float(np.median(np.diff(np.sort([r["capture_us"] for r in rows if r["camera_id"] == 0]))))
    slot = {}
    for r in rows:
        if r["camera_id"] in P and len(r["markers"]) >= N:
            slot.setdefault(round(r["capture_us"] / period), {})[r["camera_id"]] = r
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
                per[c] = [(s[c]["capture_us"] + SLOPE * pts[j][1] * t_row, pts[j][0], pts[j][1]) for j in assign]
        if len(per) >= 2: lab[k] = per
    # residual row offset on THIS data: half the median cross-camera difference in mean row time (same-LED sets)
    xr = [abs(np.mean([p[2] for p in lab[k][a]]) - np.mean([p[2] for p in lab[k][b]])) * t_row
          for k in lab for a, b in itertools.combinations(sorted(lab[k]), 2)]
    xrow_half = float(np.median(xr)) / 2
    track = {}
    for k in sorted(lab):
        for c, leds in lab[k].items():
            for led, (t, x, y) in enumerate(leds): track.setdefault((c, led), []).append((t, x, y))
    track = {key: np.array(v) for key, v in track.items()}

    def obs_at(c, led, t):
        a = track.get((c, led))
        if a is None: return None
        i = np.searchsorted(a[:, 0], t)
        if i == 0 or i >= len(a) or a[i, 0] - a[i - 1, 0] > 2.05 * period: return None
        w = (t - a[i - 1, 0]) / (a[i, 0] - a[i - 1, 0])
        return (a[i - 1, 1] + w * (a[i, 1] - a[i - 1, 1]), a[i - 1, 2] + w * (a[i, 2] - a[i - 1, 2]))

    def solve(k, dt):
        t_star = k * period + 6000.0; Xs = []
        for led in range(N):
            obs = [(c, p) for c in P if (p := obs_at(c, led, t_star + (dt if c == SKEW_CAM else 0.0))) is not None]
            if len(obs) < 2 or SKEW_CAM not in [o[0] for o in obs]: return None
            Xs.append(da.tri(P, obs)[0])
        return Xs

    ref = {k: x for k in sorted(lab) if (x := solve(k, 0.0)) is not None}
    speed = {k: float(np.linalg.norm(np.mean(ref[k + 1], 0) - np.mean(ref[k - 1], 0)) / (2 * period / 1e6)) for k in ref if k - 1 in ref and k + 1 in ref}
    pairs = list(itertools.combinations(range(N), 2))
    static = np.median([[np.linalg.norm(ref[k][i] - ref[k][j]) for i, j in pairs] for k in speed if speed[k] < 2.0], 0)
    out = {"period_us": period, "t_row_us": t_row, "skew_camera": SKEW_CAM, "slots": len(ref), "xrow_half_us": xrow_half,
           "static_d_mm": static.tolist(), "sources": []}
    for name, dt in SOURCES:
        if dt is None: dt = ST["Live FTM, camera tracking load"]["max"] + xrow_half
        R = []
        for k, v in speed.items():
            if not (20 <= v <= 800): continue
            x = solve(k, dt)
            if x is None: continue
            R.append((v, float(np.linalg.norm(np.mean(x, 0) - np.mean(ref[k], 0))),
                      float(np.abs([np.linalg.norm(x[i] - x[j]) for i, j in pairs] - static).max())))
        R = np.array(R)
        out["sources"].append({"name": name, "dt_us": dt, "n": len(R), "pos_med_mm": float(np.median(R[:, 1])), "pos_p95_mm": float(np.percentile(R[:, 1], 95)),
                               "pos_max_mm": float(R[:, 1].max()), "dist_med_mm": float(np.median(R[:, 2])), "dist_p95_mm": float(np.percentile(R[:, 2], 95)),
                               "speed_max_mm_s": float(R[:, 0].max())})
        print(f"{name:26s} dt={dt/1000:9.3f} ms n={len(R):5d} centroid shift med {np.median(R[:,1]):8.3f} p95 {np.percentile(R[:,1],95):8.3f} mm  LED-dist med {np.median(R[:,2]):7.3f} mm")
    k0 = next(iter(speed)); assert np.allclose(solve(k0, 0.0)[0], ref[k0][0])   # zero offset reproduces the reference
    json.dump(out, open(out_p, "w"), indent=1)


if __name__ == "__main__":
    main()
