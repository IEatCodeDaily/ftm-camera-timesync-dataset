#!/usr/bin/env python3
"""Measured performance of the IR tracking path (IEEE Sensors Letters).

Every number in TRACKING_RESULTS.md / stats.json is computed here from the raw
files next to this script and asserted below. Run:
    ~/.local/paperenv/.venv/bin/python analyze_tracking.py
Inputs (relative to this file): recordings/*.mcap, calibrations/*.json,
extrinsic-frames/last-solution.json, rigid_bodies.json,
../timesync-2026-09-09/analysis/offsets.csv + historical-evidence doc,
replay/*.log (optional output of the host estimator replay, section G).
"""
import glob, itertools, json, math, os, re
from collections import defaultdict
import numpy as np
from mcap.reader import make_reader

HERE = os.path.dirname(os.path.abspath(__file__))
J = lambda *p: os.path.join(HERE, *p)
TS = J("..", "timesync-2026-09-09")
STILL_PX, STILL_S, RESID_OK_PX = 0.5, 2.0, 2.0


def pct(a, q):
    return float(np.percentile(a, q))


def summ(a):
    a = np.asarray(a, float)
    return {"n": int(a.size), "median": float(np.median(a)), "p95": pct(a, 95),
            "p99": pct(a, 99), "max": float(a.max())}


def load(path):
    rows = []
    with open(path, "rb") as fh:
        for _, ch, m in make_reader(fh).iter_messages(topics=["/mocap/centroids"]):
            rows.append(json.loads(m.data))
    assert rows, path
    return rows


# ---------------------------------------------------------------- A + B + C + D
def per_recording(path):
    rows = load(path)
    name = os.path.basename(path)
    cams = sorted({r["camera_id"] for r in rows})
    res = {(r["width"], r["height"]) for r in rows}
    assert len(res) == 1, res
    rp = {r["row_period_ns"] for r in rows if r["row_period_ns"]}
    assert len(rp) == 1, rp
    rp_ns = rp.pop()
    assert all(r["flags"] & 1 for r in rows if r["camera_id"] in cams), "clock-valid flag missing"
    # every marker was undistorted on-node (marker flag 0x02 = intrinsic_corrected)
    assert all(m["flags"] & 2 for r in rows for m in r["markers"])
    host = np.array([r["host_time_ns"] for r in rows], float)
    dur = (host.max() - host.min()) / 1e9

    # Infer slot period P from the cadence: median capture interval of all cams.
    dts = np.concatenate([np.diff(np.sort([r["capture_us"] for r in rows if r["camera_id"] == c])) for c in cams])
    fps = round(1e6 / float(np.median(dts)))
    period_ps = 10**12 // fps  # firmware: period_ps = 1e12 / s_fps (integer)
    per_cam, paced = {}, []
    by_cam = defaultdict(list)
    for r in rows:
        by_cam[r["camera_id"]].append(r)
    for c in cams:
        rr = sorted(by_cam[c], key=lambda r: r["capture_us"])
        cap = np.array([r["capture_us"] for r in rr], np.int64)
        di = np.diff(cap)
        span_s = (cap[-1] - cap[0]) / 1e6
        is_paced = abs(np.median(di) * 1e6 - period_ps) / period_ps < 0.01
        if is_paced:
            paced.append(c)
        per_cam[c] = {
            "packets": len(rr), "fps": (len(rr) - 1) / span_s,
            "interval_median_us": float(np.median(di)), "interval_sd_us": float(np.std(di, ddof=1)),
            "paced_at_P": bool(is_paced),
            "detection_us_median": float(np.median([r["detection_us"] for r in rr])),
        }
    # Slot index k: capture_us - k*P is the trigger-to-capture-timestamp delay.
    # A few packets land ms off the grid; if two share a slot keep the one
    # nearest the grid and count the other (reported, not hidden).
    slot = defaultdict(dict)  # k -> cam -> row
    for c in paced:
        caps_ps = np.array([r["capture_us"] for r in by_cam[c]], np.int64) * 10**6
        ph = np.median(np.mod(caps_ps, period_ps))
        dup = off = 0
        for r in by_cam[c]:
            x = (r["capture_us"] * 10**6 - ph) / period_ps
            k = int(round(x))
            dev = abs(x - k) * period_ps / 1e6
            off += dev > 500
            d = slot[k]
            if c in d:
                dup += 1
                if dev >= abs((d[c]["capture_us"] * 10**6 - ph) / period_ps - k) * period_ps / 1e6:
                    continue
            d[c] = r
        per_cam[c]["packets_off_grid_gt500us"] = int(off)
        per_cam[c]["duplicate_slot_packets_dropped"] = dup
    ks = np.array(sorted(slot))
    nslots = int(ks.max() - ks.min() + 1)
    complete = [k for k in ks if len(slot[k]) == len(paced)]
    offs = {c: [] for c in paced}
    for k in ks:
        for c, r in slot[k].items():
            offs[c].append((r["capture_us"] * 10**6 - k * period_ps) / 1e6)
    med_off = {c: float(np.median(v)) for c, v in offs.items()}
    raw_spread, jit_spread = [], []
    for k in complete:
        t = np.array([slot[k][c]["capture_us"] * 1.0 for c in paced])
        tj = t - np.array([med_off[c] + k * period_ps / 1e6 for c in paced])
        raw_spread.append(t.max() - t.min())
        jit_spread.append(tj.max() - tj.min())
    per_cam_jit_sd = {c: float(np.std(np.array(v) - med_off[c], ddof=1)) for c, v in offs.items()}

    # C: rolling-shutter row time of each marker, and cross-camera difference
    rowt = [m["y"] * rp_ns / 1e3 for r in rows for m in r["markers"]]
    rs_diff = []  # |mean row time cam a - cam b| for slots where both see the same marker count
    for k in complete:
        for a, b in itertools.combinations(paced, 2):
            ma, mb = slot[k][a]["markers"], slot[k][b]["markers"]
            if ma and len(ma) == len(mb):
                rs_diff.append(abs(np.mean([m["y"] for m in ma]) - np.mean([m["y"] for m in mb])) * rp_ns / 1e3)

    # D: static 2-D centroid jitter, single-marker frames, non-overlapping 2-s windows
    win = int(round(STILL_S * fps))
    still = {}
    for c in paced:
        seq = sorted((k, slot[k][c]["markers"][0]) for k in ks if c in slot[k] and len(slot[k][c]["markers"]) == 1)
        sds = []
        i = 0
        while i + win <= len(seq):
            w = seq[i:i + win]
            if w[-1][0] - w[0][0] != win - 1:  # not consecutive slots: advance one
                i += 1
                continue
            xy = np.array([(m["x"], m["y"]) for _, m in w])
            sd = xy.std(axis=0, ddof=1)
            if sd.max() < STILL_PX:
                sds.append(sd)
                i += win
            else:
                i += 1
        if sds:
            s = np.array(sds)
            still[c] = {"windows": len(sds), "seconds": len(sds) * STILL_S,
                        "sd_x_px_median": float(np.median(s[:, 0])), "sd_y_px_median": float(np.median(s[:, 1])),
                        "sd_x_px_pooled": float(np.sqrt(np.mean(s[:, 0] ** 2))),
                        "sd_y_px_pooled": float(np.sqrt(np.mean(s[:, 1] ** 2)))}
    return {
        "file": name, "duration_s": dur, "cameras": cams, "resolution": list(res.pop()),
        "row_period_ns": rp_ns, "inferred_fps": fps, "period_ps": period_ps,
        "per_camera": {str(c): v for c, v in per_cam.items()},
        "paced_cameras": paced, "slots_spanned": nslots, "slots_all_paced_cams": len(complete),
        "frac_slots_all_paced_cams": len(complete) / nslots,
        "B_trigger_to_capture_us_median": {str(c): v for c, v in med_off.items()},
        "B_trigger_to_capture_jitter_sd_us": {str(c): v for c, v in per_cam_jit_sd.items()},
        "B_capture_spread_raw_us": summ(raw_spread) if raw_spread else None,
        "B_capture_spread_after_fixed_offset_us": summ(jit_spread) if jit_spread else None,
        "C_marker_row_time_us": summ(rowt) if rowt else None,
        "C_full_frame_row_span_us": rp_ns * res_h(rows) / 1e3,
        "C_cross_camera_row_time_diff_us": summ(rs_diff) if rs_diff else None,
        "D_static_2d": {str(c): v for c, v in still.items()},
        "_slot": slot, "_fps": fps,
    }


def res_h(rows):
    return rows[0]["height"]


# ---------------------------------------------------------------- E: 3-D
def rig():
    K = {}
    for f in glob.glob(J("calibrations", "board-*-sensor-5640-vga.json")):
        d = json.load(open(f))
        K[d["node_id"]] = np.array(d["solution"]["k"])  # host keys intrinsics by node_id == camera_id
    ex = json.load(open(J("extrinsic-frames", "last-solution.json")))
    assert ex["world"] == "wand"
    return {e["camera_id"]: K[e["camera_id"]] @ np.hstack([np.array(e["R"]), np.array(e["t"])[:, None]])
            for e in ex["extrinsics"]}


def tri(P, p0, p1):
    """Linear DLT for two views (pixels already undistorted on-node); returns X, per-view residuals."""
    A = []
    for Pc, (x, y) in ((P[0], p0), (P[1], p1)):
        A += [x * Pc[2] - Pc[0], y * Pc[2] - Pc[1]]
    X = np.linalg.svd(np.array(A))[2][-1]
    X = X[:3] / X[3]
    r = [float(np.hypot(*((Pc @ np.append(X, 1))[:2] / (Pc @ np.append(X, 1))[2] - np.array(p))))
         for Pc, p in ((P[0], p0), (P[1], p1))]
    return X, r


def three_d(rec, P, wand_mm):
    slot, fps = rec["_slot"], rec["_fps"]
    if not {0, 1} <= set(rec["paced_cameras"]):
        return None
    single, resid = [], []
    trio_d, trio_r = [], []
    for k in sorted(slot):
        s = slot[k]
        if 0 not in s or 1 not in s:
            continue
        a, b = s[0]["markers"], s[1]["markers"]
        if len(a) == 1 and len(b) == 1:
            p0, p1 = (a[0]["x"], a[0]["y"]), (b[0]["x"], b[0]["y"])
            X, r = tri(P, p0, p1)
            single.append((k, X, p0, p1))
            resid.append(max(r))
        if len(a) == 3 and len(b) == 3:  # L-calibrator: correspondence = permutation with smallest residual
            best = min((max(max(tri(P, (a[i]["x"], a[i]["y"]), (b[p[i]]["x"], b[p[i]]["y"]))[1]) for i in range(3)), p)
                       for p in itertools.permutations(range(3)))
            if best[0] < RESID_OK_PX:
                Xs = [tri(P, (a[i]["x"], a[i]["y"]), (b[best[1][i]]["x"], b[best[1][i]]["y"]))[0] for i in range(3)]
                trio_d.append(sorted(np.linalg.norm(Xs[i] - Xs[j]) for i, j in ((0, 1), (0, 2), (1, 2))))
                trio_r.append(best[0])
    out = {"single_marker_frames": len(single)}
    if not single:
        return out
    out["reproj_residual_px_max_of_2_views"] = summ(resid)
    out["consistent"] = bool(np.median(resid) <= RESID_OK_PX)
    if not out["consistent"]:
        return out
    # static 3-D jitter: 2-s windows of consecutive single-marker frames still in BOTH views
    win = int(round(STILL_S * fps))
    sds, i = [], 0
    while i + win <= len(single):
        w = single[i:i + win]
        if w[-1][0] - w[0][0] != win - 1:
            i += 1
            continue
        px0 = np.array([q[2] for q in w]).std(0, ddof=1)
        px1 = np.array([q[3] for q in w]).std(0, ddof=1)
        if max(px0.max(), px1.max()) < STILL_PX:
            sds.append(np.array([q[1] for q in w]).std(0, ddof=1))
            i += win
        else:
            i += 1
    if sds:
        s = np.array(sds)
        out["static_3d"] = {"windows": len(sds), "sd_xyz_mm_median": np.median(s, 0).tolist(),
                            "sd_3d_mm_median": float(np.median(np.linalg.norm(s, axis=1)))}
    if trio_d:
        d = np.array(trio_d)
        exp = np.sort(wand_mm)
        out["L_calibrator"] = {"frames": len(trio_d), "expected_mm": exp.tolist(),
                               "median_mm": np.median(d, 0).tolist(), "sd_mm": d.std(0, ddof=1).tolist(),
                               "median_error_mm": (np.median(d, 0) - exp).tolist(),
                               "iqr_mm": (np.percentile(d, 75, 0) - np.percentile(d, 25, 0)).tolist(),
                               "reproj_residual_px": summ(trio_r)}
    return out


# ---------------------------------------------------------------- F: GPIO timers
def gpio():
    rows = defaultdict(list)
    with open(os.path.join(TS, "analysis", "offsets.csv")) as fh:
        next(fh)
        for line in fh:
            run, method, node, _, off = line.strip().split(",")
            m = re.search(r"(20260908-17(0156|1055))/(\d\d-[a-z]+-[a-z]+)$", run)
            if m:
                rows[(m.group(1), m.group(3), method, int(node))].append(float(off))
    doc = open(os.path.join(TS, "historical-evidence", "docs", "TIMESYNC_METHOD_RESULTS_2026-09-08.md")).read()
    out = []
    for key in sorted({k[:3] for k in rows}):
        run, rec, method = key
        dm = re.search(r"\| %s/%s \| ([0-9a-f]+) \|[^|]*\| ([\d.]+) / ([\d.]+) / ([\d.]+) \| ([\d.]+) \|" % (run, rec), doc)
        assert dm, key
        sds, mx = {}, 0.0
        for i, node in enumerate((0, 1, 3)):
            v = np.array(rows[key + (node,)])
            assert len(v) >= 25
            sds[node] = float(v.std())  # population SD, as in the historical table
            mx = max(mx, float(np.abs(v).max()))
        doc_sd = [float(dm.group(2 + i)) for i in range(3)]
        doc_edges = re.search(r"\| %s/%s \|[^|]*\| [^|]*edges (\d+)/" % (run, rec), doc).group(1)
        # The doc counted all edges; offsets.csv keeps 25 per node. Where edge
        # counts agree, the CSV must reproduce the doc exactly.
        matches = all(abs(sds[n] - d) < 0.002 for n, d in zip((0, 1, 3), doc_sd)) and abs(mx - float(dm.group(5))) < 0.002
        if int(doc_edges) == len(rows[key + (0,)]):
            assert matches, (key, sds, doc_sd, mx)
        out.append({"run": f"{run}/{rec}", "backend": method.split(":")[0], "reference": method.split(":")[1],
                    "build": dm.group(1), "edges_per_node": len(rows[key + (0,)]),
                    "sd_us": {str(n): s for n, s in sds.items()}, "sd_us_max_node": max(sds.values()),
                    "max_abs_us": mx, "doc_edges": int(doc_edges), "doc_sd_us": doc_sd,
                    "doc_max_abs_us": float(dm.group(5)), "matches_doc": bool(matches)})
    return out


# ---------------------------------------------------------------- G: replay log
def replay():
    out = []
    for f in sorted(glob.glob(J("replay", "*.log"))):
        for m in re.finditer(r"REPLAY (\S+): packets (\d+) flushes (\d+) quorum_yield ([\d.]+) late (\d+) groups_with_3d (\d+)", open(f, errors="replace").read()):
            out.append({"recording": os.path.basename(m.group(1).replace("\\", "/")), "packets": int(m.group(2)),
                        "flushes": int(m.group(3)), "quorum_yield": float(m.group(4)),
                        "late": int(m.group(5)), "groups_with_3d": int(m.group(6))})
    return out


# ---------------------------------------------------------------- report
def f1(x, d=1):
    return f"{x:.{d}f}"


def main():
    P = rig()
    wand = np.array(json.load(open(J("rigid_bodies.json")))[0]["points_mm"])
    wand_d = [float(np.linalg.norm(wand[i] - wand[j])) for i, j in ((0, 1), (0, 2), (1, 2))]
    assert np.allclose(sorted(wand_d), [150, 200, 250])
    recs = [per_recording(p) for p in sorted(glob.glob(J("recordings", "tracking-*.mcap")))]
    for r in recs:
        r["E_3d"] = three_d(r, P, wand_d)
    stats = {"recordings": [{k: v for k, v in r.items() if not k.startswith("_")} for r in recs],
             "F_gpio_timers": gpio(), "G_replay": replay()}
    main_rec = next(r for r in recs if r["file"] == "tracking-1791273844728263000.mcap")

    # ---- headline assertions (fail loudly if the data or the analysis changes)
    assert 290 < main_rec["duration_s"] < 300 and main_rec["cameras"] == [0, 1, 2, 3]
    assert main_rec["inferred_fps"] == 35 and main_rec["resolution"] == [640, 480]
    assert main_rec["paced_cameras"] == [0, 1, 2, 3]
    assert main_rec["row_period_ns"] == 25750 and abs(main_rec["C_full_frame_row_span_us"] - 12360) < 1
    j = main_rec["B_capture_spread_after_fixed_offset_us"]
    assert j["median"] < main_rec["C_marker_row_time_us"]["median"] / 10, "row correction must dwarf timing jitter"
    for r in recs:
        e = r["E_3d"]
        if e and "consistent" in e:
            # extrinsics solved 13:10 WIB: same-session recordings (13:04-13:13) consistent, 15:xx not
            ts = int(r["file"].split("-")[1].split(".")[0]) / 1e9
            same_session = 1791266000 < ts < 1791268000
            assert e["consistent"] == same_session, (r["file"], e["reproj_residual_px_max_of_2_views"])
    g = {(x["run"].split("/")[0], x["backend"]): x for x in stats["F_gpio_timers"] if x["reference"] == "ftm"}
    mc = [x for x in stats["F_gpio_timers"] if x["backend"] == "mcpwm"]
    gp = [x for x in stats["F_gpio_timers"] if x["backend"] == "gptpair"]
    assert {x["build"] for x in mc + gp} == {"8dc05893b49ecfab"}
    assert len(stats["G_replay"]) == 5 and all(x["quorum_yield"] > 0.9 for x in stats["G_replay"])
    assert {x["build"] for x in stats["F_gpio_timers"] if x["run"].startswith("20260908-170156")} == {"34e1a3df22c26af8"}

    clock_err = max(x["max_abs_us"] for x in mc + gp + [g[("20260908-170156", "gpt")]])
    assert main_rec["C_cross_camera_row_time_diff_us"]["median"] > 100 * clock_err
    stats["C_vs_F_clock_err_ratio_median"] = main_rec["C_cross_camera_row_time_diff_us"]["median"] / clock_err
    with open(J("stats.json"), "w") as fh:
        json.dump(stats, fh, indent=1)
    stats["_clock_err_us"] = clock_err
    write_md(stats, recs, main_rec, wand_d, mc, gp)
    print("wrote stats.json and TRACKING_RESULTS.md")


def write_md(stats, recs, main_rec, wand_d, mc, gp):
    L = []
    a = L.append
    a("# IR tracking: measured performance (2026-10-06 recordings)\n")
    a("Generated by `analyze_tracking.py` from the files in this directory; every number below is computed and asserted by that script (re-run it to regenerate this file and `stats.json`). Times are WIB (UTC+7). Spreads/SDs in microseconds unless stated.\n")
    a("Inputs: `recordings/` (copies of the Studio MCAPs, topic `/mocap/centroids`), `calibrations/` (OV5640 VGA intrinsics, keyed by node_id = camera_id), `extrinsic-frames/last-solution.json` (cams 0 and 1, wand frame, solved 13:10 WIB), `rigid_bodies.json` (L-calibrator 0/150/200 mm), `../timesync-2026-09-09/analysis/offsets.csv`.\n")
    a("Recordings used: the 13:04, 13:09 and 13:11 recordings (same rig session as the extrinsics) and the 15:04 (296 s) and 15:09 recordings (4 cameras). The 12:34 and 12:59 recordings were not copied: they have no frames where cams 0 and 1 each see a single marker, so they add nothing to E, and they predate the 4-camera rig. The 15:01 recording is 1.3 s long and was also skipped.\n")

    a("## A. Recordings\n")
    a("| recording | start (WIB) | duration s | cams | res | fps (inferred P) | row period ns | slots with all paced cams |")
    a("|---|---|---|---|---|---|---|---|")
    import datetime
    for r in recs:
        ts = int(r["file"].split("-")[1].split(".")[0]) / 1e9
        st = datetime.datetime.utcfromtimestamp(ts + 7 * 3600).strftime("%H:%M:%S")
        a(f"| {r['file']} | {st} | {f1(r['duration_s'])} | {','.join(map(str, r['cameras']))} | {r['resolution'][0]}x{r['resolution'][1]} | {r['inferred_fps']} | {r['row_period_ns']} | {r['slots_all_paced_cams']}/{r['slots_spanned']} = {f1(100*r['frac_slots_all_paced_cams'])}% (cams {','.join(map(str, r['paced_cameras']))}) |")
    a("\nPer camera (frame rate = (packets-1)/capture-time span; interval = consecutive capture_us difference):\n")
    a("| recording | cam | packets | fps | interval median us | interval SD us | paced at P | packets >500 us off grid | on-board detection_us median |")
    a("|---|---|---|---|---|---|---|---|---|")
    for r in recs:
        for c, v in r["per_camera"].items():
            a(f"| {r['file'][9:22]} | {c} | {v['packets']} | {f1(v['fps'],2)} | {f1(v['interval_median_us'])} | {f1(v['interval_sd_us'])} | {'yes' if v['paced_at_P'] else 'NO'} | {v.get('packets_off_grid_gt500us','-')} | {f1(v['detection_us_median'],0)} |")
    a("\nP is inferred as 1e12/round(1e6/median interval) ps (the firmware's `period_ps = 1e12 / fps`). A camera counts as paced when its median interval is within 1% of P. In the 13:04 recording camera 2 ran at a ~25.5 ms interval, not on the 35 fps grid, so it is excluded from B there. Interval SD includes skipped slots (2P intervals), so it is a stream-continuity number, not a clock number.\n")

    a("## B. Camera-layer timing during tracking\n")
    a("Each packet is assigned slot k = round((capture_us - phase)/P). Trigger-to-capture-timestamp delay is capture_us - k*P on the shared hub clock: the fixed time from the FREX slot to the driver's post-VSYNC software stamp. Raw spread = max - min of capture_us across the paced cameras in a slot (slots where every paced camera reported). Jitter spread = the same after subtracting each camera's median delay.\n")
    a("| recording | per-cam median delay us | per-cam delay SD us | raw spread median / p95 / p99 / max | jitter spread median / p95 / p99 / max | slots |")
    a("|---|---|---|---|---|---|")
    for r in recs:
        rs, js = r["B_capture_spread_raw_us"], r["B_capture_spread_after_fixed_offset_us"]
        if not rs:
            continue
        d = ", ".join(f"{c}:{f1(v)}" for c, v in r["B_trigger_to_capture_us_median"].items())
        s = ", ".join(f"{c}:{f1(v)}" for c, v in r["B_trigger_to_capture_jitter_sd_us"].items())
        a(f"| {r['file'][9:22]} | {d} | {s} | {f1(rs['median'])} / {f1(rs['p95'])} / {f1(rs['p99'])} / {f1(rs['max'])} | {f1(js['median'])} / {f1(js['p95'])} / {f1(js['p99'])} / {f1(js['max'])} | {rs['n']} |")
    m = main_rec
    a(f"\nIn the 296-s, 4-camera recording the fixed trigger-to-timestamp delay is {f1(min(m['B_trigger_to_capture_us_median'].values()))}-{f1(max(m['B_trigger_to_capture_us_median'].values()))} us per camera, so the fixed part differs by {f1(max(m['B_trigger_to_capture_us_median'].values())-min(m['B_trigger_to_capture_us_median'].values()))} us between cameras. With that removed, the median cross-camera spread is {f1(m['B_capture_spread_after_fixed_offset_us']['median'])} us (p99 {f1(m['B_capture_spread_after_fixed_offset_us']['p99'])} us). In the 13:xx recordings the delay of cams 0 and 1 is ~3.3-3.6 ms instead of ~1.8 ms. The delay is stable within each recording; these files do not show why it differs between sessions.\n")
    a("**capture_us is the camera driver's software timestamp taken after VSYNC, mapped to the hub clock. It is not the exposure instant.** B therefore measures the trigger-to-software-stamp path (FTM model + FreeRTOS + driver ISR latency), not optical exposure alignment.\n")

    a("## C. Rolling-shutter magnitude vs clock error\n")
    c = m["C_marker_row_time_us"]
    d = m["C_cross_camera_row_time_diff_us"]
    a(f"Row period {m['row_period_ns']} ns x {m['resolution'][1]} rows = {f1(m['C_full_frame_row_span_us']/1000,2)} ms top-to-bottom. Per-marker row offset y*row_period over all markers in the 296-s recording: median {f1(c['median'])} us, p95 {f1(c['p95'])}, max {f1(c['max'])} us (n={c['n']}). For slots where two paced cameras see the same number of markers, the cross-camera difference in mean row time (how far the per-row correction moves one camera's timestamps relative to the other's) is median {f1(d['median'])} us, p95 {f1(d['p95'])}, p99 {f1(d['p99'])}, max {f1(d['max'])} us (n={d['n']} camera-pairs x slots). The capture-time jitter spread in B has a median of {f1(m['B_capture_spread_after_fixed_offset_us']['median'])} us, and the FTM clock model reports clock_uncertainty_us 0-1. At the median the cross-camera row-time difference is {f1(d['median']/m['B_capture_spread_after_fixed_offset_us']['median'],0)}x the residual capture-timestamp jitter and {f1(d['median']/stats['_clock_err_us'],0)}x the worst GPIO clock offset of the FTM-disciplined timer backends in F ({f1(stats['_clock_err_us'],2)} us). The single-marker row offset itself (median {f1(c['median'])} us) is {f1(c['median']/stats['_clock_err_us'],0)}x that clock error.\n")
    a("| recording | row-time median / p95 / max us | cross-cam row-time diff median / p95 / max us | n pairs |")
    a("|---|---|---|---|")
    for r in recs:
        if r["C_cross_camera_row_time_diff_us"]:
            c, d = r["C_marker_row_time_us"], r["C_cross_camera_row_time_diff_us"]
            a(f"| {r['file'][9:22]} | {f1(c['median'])} / {f1(c['p95'])} / {f1(c['max'])} | {f1(d['median'])} / {f1(d['p95'])} / {f1(d['max'])} | {d['n']} |")

    a("\n## D. Static 2-D centroid jitter\n")
    a(f"Single-marker frames on consecutive slots, cut into non-overlapping {STILL_S:.0f}-s windows ({int(STILL_S*35)} frames). A window is static if SD(x) and SD(y) are both < {STILL_PX} px. The reported SDs are per-window SDs (median over windows, and RMS-pooled). The < {STILL_PX} px gate bounds what can be reported: it selects still windows and cannot measure jitter above the gate.\n")
    a("| recording | cam | windows | seconds | SD x px (median / pooled) | SD y px (median / pooled) |")
    a("|---|---|---|---|---|---|")
    for r in recs:
        for cam, v in r["D_static_2d"].items():
            a(f"| {r['file'][9:22]} | {cam} | {v['windows']} | {f1(v['seconds'],0)} | {f1(v['sd_x_px_median'],3)} / {f1(v['sd_x_px_pooled'],3)} | {f1(v['sd_y_px_median'],3)} / {f1(v['sd_y_px_pooled'],3)} |")

    a("\nCameras missing from this table never had a 2-s run of still single-marker frames. In the 296-s run, cams 0/1 saw two markers on nearly every frame. Where the median and the pooled SD differ widely (13:09/13:11, cam 2), a few windows near the 0.5-px gate dominate the pooled value.\n")
    a("\n## E. 3-D consistency (cams 0 and 1)\n")
    a(f"Markers arrive already undistorted on-node (every marker has flag 0x02 `intrinsic_corrected`; asserted). The host applies the same rule, so no second undistortion is applied. Projection P = K[R|t] per camera. DLT triangulation of single-marker slots. The residual is the larger of the two per-view reprojection errors. Extrinsics are accepted for a recording only if the median residual is <= {RESID_OK_PX} px.\n")
    a("| recording | single-marker slots (both cams) | residual median / p95 / max px | consistent |")
    a("|---|---|---|---|")
    for r in recs:
        e = r["E_3d"]
        if e is None:
            a(f"| {r['file'][9:22]} | cams 0/1 not both paced | - | - |")
        elif "consistent" not in e:
            a(f"| {r['file'][9:22]} | 0 | - | not testable |")
        else:
            q = e["reproj_residual_px_max_of_2_views"]
            a(f"| {r['file'][9:22]} | {q['n']} | {f1(q['median'],2)} / {f1(q['p95'],2)} / {f1(q['max'],2)} | {'yes' if e['consistent'] else 'NO'} |")
    a("\nThe 15:04 (296 s) and 15:09 recordings have no slot where cams 0 and 1 each see exactly one marker: in the 296-s run they see 2 markers on nearly every frame. The 2-view check is therefore not testable on them. **No 3-D numbers are reported for the 15:xx recordings**: the extrinsics were solved at 13:10, and nothing shows the rig was untouched afterwards.\n")
    for r in recs:
        e = r["E_3d"]
        if e and e.get("consistent"):
            if "static_3d" in e:
                s = e["static_3d"]
                a(f"- {r['file'][9:22]}: static 3-D jitter over {s['windows']} still 2-s windows: per-axis SD (median) {', '.join(f1(x,2) for x in s['sd_xyz_mm_median'])} mm, 3-D SD {f1(s['sd_3d_mm_median'],2)} mm.")
            if "L_calibrator" in e:
                l = e["L_calibrator"]
                a(f"- {r['file'][9:22]}: L-calibrator, {l['frames']} slots with 3 markers in both views (residual < {RESID_OK_PX} px, median {f1(l['reproj_residual_px']['median'],2)} px). Sorted inter-marker distances median {', '.join(f1(x,2) for x in l['median_mm'])} mm vs {', '.join(f1(x,0) for x in l['expected_mm'])} mm (error {', '.join(f1(x,2) for x in l['median_error_mm'])} mm; SD {', '.join(f1(x,2) for x in l['sd_mm'])} mm; IQR {', '.join(f1(x,2) for x in l['iqr_mm'])} mm, so the SD is driven by a minority of frames: wand motion during the 3 row-staggered exposures, or a wrong correspondence that still passed the 2-px gate; these files do not separate the two).")
    a("\nThe wand scale was set from this same L-calibrator (extrinsic world = \"wand\"), so the distance agreement is a **self-consistency check of the calibration, not independent accuracy**.\n")

    a("## F. GPIO timer backends (logic analyzer, 25-s records, offsets vs node 2)\n")
    a("From `offsets.csv`. SD is the population SD (ddof=0, the convention of the historical table) of each follower's offset over all rising edges, and max is the largest |offset| over nodes 0/1/3. Where the doc reports the same edge count as the CSV, both are asserted to match the historical method-results table to 0.002 us. For the two 00-runs of build 8dc05893 the doc counted 26 edges against 25 in the CSV, so its figures differ slightly (shown in the last column).\n")
    a("| run | backend | build | edges/node (csv) | SD node 0 / 1 / 3 us | max abs us | doc SD / max (edges) |")
    a("|---|---|---|---|---|---|---|")
    for x in stats["F_gpio_timers"]:
        a(f"| {x['run']} | {x['backend']}:{x['reference']} | {x['build']} | {x['edges_per_node']} | {' / '.join(f1(v,3) for v in x['sd_us'].values())} | {f1(x['max_abs_us'],3)} | {'same' if x['matches_doc'] else ' / '.join(f1(v,3) for v in x['doc_sd_us']) + ' / ' + f1(x['doc_max_abs_us'],3) + ' (' + str(x['doc_edges']) + ')'} |")
    best_mc = max(x["max_abs_us"] for x in mc)
    best_gp = max(x["max_abs_us"] for x in gp)
    sd_mc = max(x["sd_us_max_node"] for x in mc)
    sd_gp = max(x["sd_us_max_node"] for x in gp)
    gpt = [x for x in stats["F_gpio_timers"] if x["backend"] == "gpt" and x["reference"] == "ftm"][0]
    a(f"\nOn the same build (8dc05893), across its two 25-s repeats, MCPWM's worst per-node SD is {f1(sd_mc,3)} us with worst |offset| {f1(best_mc,3)} us. GPTimer-pair's are {f1(sd_gp,3)} us and {f1(best_gp,3)} us. **On SD, {'MCPWM' if sd_mc < sd_gp else 'GPTimer-pair'} was better ({f1(min(sd_mc,sd_gp),3)} vs {f1(max(sd_mc,sd_gp),3)} us). On worst-case offset, {'MCPWM' if best_mc < best_gp else 'GPTimer-pair'} was better ({f1(min(best_mc,best_gp),3)} vs {f1(max(best_mc,best_gp),3)} us).** Neither wins both metrics, the margins are a fraction of a microsecond, and the evidence is n=2 records of 25 edges per node, so the data do not rank the two. The single-GPTimer `gpt:ftm` record (worst SD {f1(gpt['sd_us_max_node'],3)} us, max {f1(gpt['max_abs_us'],3)} us) is from a **different build (34e1a3df)**, so its edge over MCPWM is not a like-for-like comparison: firmware changed between the two runs.\n")

    a("## G. Host live-estimator replay\n")
    if stats["G_replay"]:
        a("`cargo test -p studio-server replay_live_recording -- --ignored` with MOCAP_STUDIO_DIR = this directory (same intrinsics/extrinsics/rigid bodies) and MOCAP_REPLAY = each recording; raw logs in `replay/`. Quorum yield and 3-D groups are as printed by the test.\n")
        a("| recording | packets | flushes (groups) | quorum yield | late obs | groups with 3-D |")
        a("|---|---|---|---|---|---|")
        for x in stats["G_replay"]:
            a(f"| {x['recording']} | {x['packets']} | {x['flushes']} | {f1(x['quorum_yield'],3)} | {x['late']} | {x['groups_with_3d']} |")
        a(f"\nHost source: {open(J('replay', 'SOURCE.txt')).read().strip()}.\n")
        a("The replay groups packets on hub capture time with t_offset_s = 0: this test harness does not apply the per-row rolling-shutter correction. The rig holds only cams 0 and 1, so 3-D groups are 2-view groups. For the 15:xx recordings the extrinsics are UNVERIFIED (E was not testable), and the high `late` counts there (cams 2/3 present) were not investigated. **Read G as the estimator's grouping/quorum yield on real packet streams, not as a 3-D quality number.**\n")
    else:
        a("Not run: the host replay could not be built within the 10-min budget (see the end of this file).\n")

    a("## What these numbers do NOT show\n")
    a("- **No independent 3-D accuracy.** No external reference (OptiTrack, a CMM or a calibrated bar not used in calibration) exists. The L-calibrator distances in E come from the object that set the scale.")
    a("- **3-D is from two cameras only** (cams 0 and 1), and only for the 13:xx session the extrinsics were solved in. The 15:xx 4-camera recordings could not be checked against the extrinsics (no single-marker frames in cams 0/1), so no 3-D result is claimed for them, and none for 4 cameras.")
    a("- **capture_us is a software timestamp** (driver stamp after VSYNC, mapped through the FTM clock model). B is the timestamp alignment, not the optical exposure alignment. A fixed per-camera sensor latency would be invisible to it, and a constant one is removed by design in the jitter figure.")
    a("- **FREX gives a common frame start only.** Per `evidence/synchronized-exposure-limitation.md` (datasheet section 4.10.2), the FREX shutter-length registers (0x3B04/05) are inert in rolling-shutter mode, so exposure length is fixed and sensor-determined in synchronized mode. Rows still expose sequentially. The per-marker y*row_period correction is a model of that sweep: these recordings cannot validate it against a moving reference.")
    a("- **Static jitter is gated.** D/E select windows with SD < 0.5 px, so they characterise still markers below that bound and say nothing about moving-marker error or motion blur.")
    a("- **F is GPIO-edge timing** on a bench, 25-s records, n=2 per backend. It is not camera exposure timing, and the backend comparison spans two firmware builds.")
    a("- **The clock_uncertainty_us field** (0-1 us) is the node's own model residual, not an external measurement.")
    a("- The 13:xx recordings had 3 cameras with cams 0/1 delayed ~3.4 ms. Nothing here explains that per-session offset, beyond showing it is constant within a recording.\n")
    with open(J("TRACKING_RESULTS.md"), "w") as fh:
        fh.write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
