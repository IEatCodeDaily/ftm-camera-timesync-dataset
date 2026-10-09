"""Dynamic 3-D check from Dobot sweeps (offline, from the studio MCAP).

The taped calibrator is rigid, so its three inter-LED distances must not change
while it moves. For each camera, each LED's image track is interpolated to one
common instant per slot, then all cameras are triangulated together. The
instant each image row was seen is t0 + s*y*t_row with s in {0, 0.5, 1}:
s=0 ignores the rolling readout; s=0.5/1 are the two bounds of the FREX
global-reset slope. Distance error = moving distance - static distance (from
the same recording, arm stopped), so lens/scale bias cancels.

Usage: python3 dynamic_analyse.py <mcap> <out.json> seg-*.json
"""
import json, sys, glob, itertools, math
import numpy as np
from mcap.reader import make_reader

STUDIO = "/mnt/e/Projects/wireless-ir-mocap/mocap/studio"
RESID_PX = 3.0


def rig():
    K = {}
    for f in glob.glob(f"{STUDIO}/calibrations/board-*-sensor-5640-vga.json"):
        d = json.load(open(f)); K[d["node_id"]] = np.array(d["solution"]["k"])
    ex = json.load(open(f"{STUDIO}/extrinsic-frames/last-solution.json"))
    return {e["camera_id"]: K[e["camera_id"]] @ np.hstack([np.array(e["R"]), np.array(e["t"])[:, None]]) for e in ex["extrinsics"]}


def tri(P, obs):
    A = []
    for c, (x, y) in obs:
        A += [x * P[c][2] - P[c][0], y * P[c][2] - P[c][1]]
    X = np.linalg.svd(np.array(A))[2][-1]; X = X[:3] / X[3]
    r = [math.hypot(*((P[c] @ np.append(X, 1))[:2] / (P[c] @ np.append(X, 1))[2] - np.array(p))) for c, p in obs]
    return X, max(r)


def load(path, t_lo, t_hi):
    """MCAP from the studio, or the dataset's centroid subset (.jsonl.gz, one row per line)."""
    if path.endswith(".jsonl.gz"):
        import gzip
        return [r for r in map(json.loads, gzip.open(path, "rt")) if t_lo <= r["host_time_ns"] <= t_hi]
    rows = []
    with open(path, "rb") as fh:
        for _, _, m in make_reader(fh).iter_messages(topics=["/mocap/centroids"], start_time=t_lo, end_time=t_hi, log_time_order=False):
            rows.append(json.loads(m.data))
    return rows


def label3(X):
    """Order three points as (corner, short-arm end, long-arm end) by the L geometry."""
    d = {(i, j): np.linalg.norm(X[i] - X[j]) for i, j in itertools.combinations(range(3), 2)}
    far = max(d, key=d.get); corner = ({0, 1, 2} - set(far)).pop()
    a, b = far
    a, b = (a, b) if np.linalg.norm(X[a] - X[corner]) < np.linalg.norm(X[b] - X[corner]) else (b, a)
    return [corner, a, b]


def main():
    mcap, out = sys.argv[1], sys.argv[2]
    segs = []
    for f in sys.argv[3:]:
        s = json.load(open(f)); segs += [dict(x, label=s["label"], speed_pct=s["speed_pct"]) for x in s["segments"]]
    t_lo, t_hi = min(x["t0_ns"] for x in segs) - 5_000_000_000, max(x["t1_ns"] for x in segs) + 5_000_000_000
    rows = load(mcap, t_lo, t_hi)
    assert rows, "no rows in window"
    P = rig()
    rp = {r["row_period_ns"] for r in rows}; assert len(rp) == 1; t_row = rp.pop() / 1000.0  # us
    dts = np.diff(np.sort([r["capture_us"] for r in rows if r["camera_id"] == 0]))
    period = float(np.median(dts))
    slot = {}
    for r in rows:
        if r["camera_id"] in P and len(r["markers"]) == 3:
            slot.setdefault(round(r["capture_us"] / period), {})[r["camera_id"]] = r
    host_of = {k: min(r["host_time_ns"] for r in s.values()) for k, s in slot.items()}

    # 1) label LEDs per slot (slope-independent): triangulate with the best camera pair, reproject.
    lab = {}  # k -> {cam: [(x,y) for corner, short, long], 't0': {cam: capture_us}}
    for k, s in slot.items():
        cams = sorted(s)
        if len(cams) < 2:
            continue
        a, b = cams[0], cams[1]
        ma, mb = s[a]["markers"], s[b]["markers"]
        best = min((max(tri(P, [(a, (ma[i]["x"], ma[i]["y"])), (b, (mb[p[i]]["x"], mb[p[i]]["y"]))])[1] for i in range(3)), p)
                   for p in itertools.permutations(range(3)))
        if best[0] > RESID_PX:
            continue
        X = [tri(P, [(a, (ma[i]["x"], ma[i]["y"])), (b, (mb[best[1][i]]["x"], mb[best[1][i]]["y"]))])[0] for i in range(3)]
        order = label3(X)
        Xo = [X[i] for i in order]
        per = {}
        for c in cams:
            pts = [(m["x"], m["y"]) for m in s[c]["markers"]]
            assign = []
            for Xi in Xo:
                q = P[c] @ np.append(Xi, 1); q = q[:2] / q[2]
                j = min(range(3), key=lambda j: math.hypot(pts[j][0] - q[0], pts[j][1] - q[1]))
                assign.append(j)
            if len(set(assign)) == 3:
                per[c] = ([pts[j] for j in assign], s[c]["capture_us"])
        if len(per) >= 2:
            lab[k] = per
    ks = sorted(lab)

    def at(k, c, led, slope, t_star):
        """Image position of `led` in camera c interpolated to t_star from slots k-1..k+1."""
        samp = []
        for kk in (k - 1, k, k + 1):
            if kk in lab and c in lab[kk]:
                (pts, t0) = lab[kk][c]; x, y = pts[led]
                samp.append((t0 + slope * y * t_row, x, y))
        if len(samp) < 2:
            return None
        samp.sort()
        for (ta, xa, ya), (tb, xb, yb) in zip(samp, samp[1:]):
            if ta <= t_star <= tb and tb > ta:
                w = (t_star - ta) / (tb - ta); return (xa + w * (xb - xa), ya + w * (yb - ya))
        return None

    res = {}
    for slope in (0.0, 0.5, 1.0):
        recs = []
        for k in ks:
            t_star = k * period + 6000.0  # mid-readout of slot k on the shared clock
            Xs, rmax = [], 0.0
            for led in range(3):
                obs = [(c, p) for c in lab[k] if (p := at(k, c, led, slope, t_star)) is not None]
                if len(obs) < 2:
                    break
                X, r = tri(P, obs); Xs.append(X); rmax = max(rmax, r)
            if len(Xs) == 3 and rmax < RESID_PX:
                recs.append((k, t_star, Xs, rmax, len(obs)))
        # velocity of the corner LED from neighbours (central difference)
        pos = {k: Xs[0] for k, _, Xs, _, _ in recs}
        out_rows = []
        for k, t, Xs, rmax, n in recs:
            if k - 1 in pos and k + 1 in pos:
                vv = (pos[k + 1] - pos[k - 1]) / (2 * period / 1e6)
                v = float(np.linalg.norm(vv))
                d = [float(np.linalg.norm(Xs[i] - Xs[j])) for i, j in ((0, 1), (0, 2), (1, 2))]
                out_rows.append({"k": k, "host_ns": host_of[k], "speed_mm_s": v, "vel": vv.tolist(), "pos": Xs[0].tolist(),
                                 "d_mm": d, "resid_px": rmax, "n_cams": n})
        res[str(slope)] = out_rows
    # static reference distances: slots with speed < 2 mm/s at slope 0
    st = np.array([r["d_mm"] for r in res["0.0"] if r["speed_mm_s"] < 2.0])
    ref = np.median(st, 0)
    summary = {"t_row_us": t_row, "period_us": period, "slots_labelled": len(ks), "static_n": len(st),
               "static_d_mm": ref.tolist(), "static_d_sd_mm": st.std(0, ddof=1).tolist(), "bins": {}}
    bins = [(2, 50), (50, 100), (100, 150), (150, 200), (200, 300), (300, 1000)]
    for s, rr in res.items():
        b_out = []
        for lo, hi in bins:
            e = np.array([np.array(r["d_mm"]) - ref for r in rr if lo <= r["speed_mm_s"] < hi])
            if len(e) < 10:
                continue
            ae = np.abs(e).max(1)
            b_out.append({"speed_lo": lo, "speed_hi": hi, "n": len(e), "rms_mm": float(np.sqrt((e ** 2).mean())),
                          "median_abs_max_mm": float(np.median(ae)), "p95_abs_max_mm": float(np.percentile(ae, 95)),
                          "per_pair_rms_mm": np.sqrt((e ** 2).mean(0)).tolist()})
        summary["bins"][s] = b_out
    summary["max_speed_mm_s"] = float(max(r["speed_mm_s"] for r in res["0.0"]))
    json.dump({"summary": summary, "rows": res}, open(out, "w"))
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
