"""Calibrator walk-through length test (2026-10-19). Input: replay CSVs (t,id,x,y,z, confirmed 3-D
tracks from Studio's estimator, pixel-bias learning off). Per frame take the track triple whose
sorted sides best fit the calibrator design (149.5 / 196.5 mm, right angle), accept it only if
(a) every side is within GATE mm, (b) the angle at the shared vertex is 90 +- 5 deg, and (c) the same
three track ids hold it for >= MIN_RUN frames (rejects chance triangles of other LEDs).
Error statistics are weighted per 100-mm cell (one median per occupied cell) so a long rest in one
spot counts once. Usage: python walk_lengths.py out.json csv..."""
import sys, json, itertools, collections, numpy as np
TRUE = np.array([149.5, 196.5, float(np.hypot(149.5, 196.5))]); GATE, MIN_RUN, CELL = 15.0, 18, 100.0
acc = []
for f in sys.argv[2:]:
    D = np.loadtxt(f, delimiter=",", ndmin=2)
    fr = collections.defaultdict(dict)
    for t, i, x, y, z in D: fr[round(t, 3)][int(i)] = np.array([x, y, z])
    cand = []
    for t in sorted(fr):
        pts = fr[t]; best = None
        for ids in itertools.combinations(sorted(pts), 3):
            T = [pts[i] for i in ids]
            sides = sorted((np.linalg.norm(T[a] - T[b]), a, b) for a, b in ((0, 1), (1, 2), (0, 2)))
            s = np.array([x[0] for x in sides]); e = np.abs(s - TRUE).max()
            if e > GATE: continue
            v = ({0, 1, 2} - {sides[2][1], sides[2][2]}).pop()        # vertex opposite the diagonal
            u1, u2 = [T[k] - T[v] for k in (0, 1, 2) if k != v]
            ang = np.degrees(np.arccos(np.clip(u1 @ u2 / np.linalg.norm(u1) / np.linalg.norm(u2), -1, 1)))
            if abs(ang - 90) > 5: continue
            if best is None or e < best[0]: best = (e, ids, s, np.mean(T, 0))
        cand.append((t, best))
    run = []
    def flush():
        if len(run) >= MIN_RUN: acc.extend(run)
    for t, b in cand:
        if b and run and b[1] == run[-1][1]: run.append((t, b[1], b[2], b[3]))
        else:
            flush(); run = [(t, b[1], b[2], b[3])] if b else []
    flush()
S = np.array([a[2] for a in acc]); C = np.array([a[3] for a in acc]); E = S - TRUE
cells = collections.defaultdict(list)
for k, c in enumerate(C): cells[tuple((c // CELL).astype(int))].append(k)
Ec = np.array([np.median(E[v], 0) for v in cells.values()]); Cc = np.array([C[v].mean(0) for v in cells.values()])
rel = Ec / TRUE
out = {"frames": len(S), "cells_100mm": len(cells), "box_min_mm": C.min(0).round(0).tolist(), "box_max_mm": C.max(0).round(0).tolist(),
       "cell_side_err_rms_mm": float(np.sqrt((Ec ** 2).mean())), "cell_side_err_abs_p95_mm": float(np.percentile(abs(Ec), 95)),
       "cell_side_err_max_mm": float(abs(Ec).max()), "cell_rel_rms_pct": float(100 * np.sqrt((rel ** 2).mean())),
       "cell_mean_err_mm": Ec.mean(0).round(2).tolist(), "frame_side_err_rms_mm": float(np.sqrt((E ** 2).mean()))}
for k, n in enumerate(("short", "long", "diag")): out[n + "_cell_median_mm"] = float(np.median(Ec[:, k]))
json.dump({**out, "cells": [{"c": c.round(0).tolist(), "err": e.round(2).tolist()} for c, e in zip(Cc, Ec)]}, open(sys.argv[1], "w"), indent=0)
print(json.dumps(out, indent=1))
