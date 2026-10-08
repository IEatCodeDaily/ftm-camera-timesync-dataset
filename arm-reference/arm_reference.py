"""Robot-arm reference for the IR tracker (held-out validation).

The studio's arm calibration (12 grid poses) already fixed world<-robot and the
tool offset. This script visits NEW poses (a different, finer grid, plus a
repeat of each), waits until the arm has stopped, and records for each pose
~1 s of the IR tool point (`ee_ir`, from the rigid-body fit of the taped
calibrator) and the controller's reported tool point mapped to world
(`ee_reported_world`). Output: per-pose rows + summary to JSON.

Read-only w.r.t. calibration: never calls /api/arm/calibrate.
Usage: python3 arm_reference.py <out.json> [repeats]
"""
import json, sys, time, math, urllib.request, statistics as st

API = "http://127.0.0.1:3001/api"
H = {"x-mocap-studio": "1", "Content-Type": "application/json"}


def req(path, body=None):
    r = urllib.request.Request(API + path, data=None if body is None else json.dumps(body).encode(),
                               headers=H, method="GET" if body is None else "POST")
    return json.loads(urllib.request.urlopen(r, timeout=20).read() or b"null") if body is None else urllib.request.urlopen(r, timeout=20).read()


def status(): return req("/arm")


def wait_still(timeout=15.0):
    """Arm idle and its reported pose unchanged over 0.5 s."""
    t0 = time.time(); last = None
    while time.time() - t0 < timeout:
        s = status()
        p = s["pose"]["xyz"] if s.get("pose") else None
        if s["mode"] == "idle" and p and last and max(abs(a - b) for a, b in zip(p, last)) < 0.05:
            return s
        last = p; time.sleep(0.5)
    raise TimeoutError("arm did not settle")


def poses(_home):
    """Held-out poses inside the calibration volume (calibration_grid: r 175/275 mm,
    yaw -35/0/35 deg, z -5/105 mm) but never on a calibration stop: r 200/250,
    yaw -20/0/20, z 15/50/85 -> 18 poses (robot frame, mm)."""
    out = []
    for z in (15, 50, 85):
        for r in (200, 250):
            for a in (-20, 0, 20):
                t = math.radians(a); out.append([round(r * math.cos(t), 1), round(r * math.sin(t), 1), z])
    return out


if __name__ == "__main__":
    out_path = sys.argv[1]; repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    s = status()
    assert s["connected"] and s["homed"] and s.get("calibration"), "arm must be connected, homed and calibrated"
    assert s["config"]["ee_body"] == s["calibration"]["body"], "ee body changed since calibration"
    start = s["pose"]["xyz"]
    plan = poses(start)
    if len(sys.argv) > 3: plan = plan[:int(sys.argv[3])]          # dry run: first N poses
    rows = []
    for rep in range(repeats):
        for i, p in enumerate(plan if rep % 2 == 0 else plan[::-1]):        # reverse on repeats: approach from other side
            req("/arm/move", {"xyz": p})
            try:
                wait_still()
            except TimeoutError:
                rows.append({"cmd": p, "rep": rep, "error": "no settle"}); continue
            time.sleep(0.3)
            ir, rep_w, pose = [], [], None
            t_end = time.time() + 1.0
            while time.time() < t_end:
                s = status()
                if s.get("ee_seen") and s.get("ee_ir") and s.get("ee_reported_world"):
                    ir.append(s["ee_ir"]); rep_w.append(s["ee_reported_world"]); pose = s["pose"]
                time.sleep(0.05)
            if len(ir) < 5:
                rows.append({"cmd": p, "rep": rep, "error": f"ir samples {len(ir)}"}); continue
            m_ir = [st.median(c) for c in zip(*ir)]; m_rw = [st.median(c) for c in zip(*rep_w)]
            sd_ir = [st.pstdev(c) for c in zip(*ir)]
            err = [a - b for a, b in zip(m_ir, m_rw)]
            rows.append({"cmd": p, "rep": rep, "pose_xyz": pose["xyz"], "joints": pose["joints"], "n": len(ir),
                         "ir_world": m_ir, "reported_world": m_rw, "ir_sd": sd_ir, "err": err, "err_norm": math.sqrt(sum(e * e for e in err))})
            print(f"rep {rep} pose {i:2d} n {len(ir):2d} |err| {rows[-1]['err_norm']:6.2f} mm  ir sd {max(sd_ir):.2f}", flush=True)
    req("/arm/move", {"xyz": start}); wait_still()
    ok = [r for r in rows if "err" in r]
    e = sorted(r["err_norm"] for r in ok)
    summ = {"n_poses": len(plan), "repeats": repeats, "n_ok": len(ok), "n_fail": len(rows) - len(ok),
            "err_median_mm": st.median(e), "err_rms_mm": math.sqrt(sum(x * x for x in e) / len(e)), "err_max_mm": e[-1],
            "err_p95_mm": e[min(len(e) - 1, round(0.95 * (len(e) - 1)))],
            "per_axis_rms_mm": [math.sqrt(sum(r["err"][k] ** 2 for r in ok) / len(ok)) for k in range(3)],
            "static_ir_sd_median_mm": st.median(max(r["ir_sd"]) for r in ok),
            "calibration": s["calibration"], "ee_body": s["config"]["ee_body"], "t_unix": time.time()}
    # repeatability: same commanded pose, different approach direction
    by = {}
    for r in ok: by.setdefault(tuple(r["cmd"]), []).append(r["ir_world"])
    rpt = [math.dist(v[0], v[1]) for v in by.values() if len(v) >= 2]
    summ["repeat_ir_dist_median_mm"] = st.median(rpt) if rpt else None
    json.dump({"summary": summ, "rows": rows}, open(out_path, "w"), indent=1)
    print(json.dumps({k: v for k, v in summ.items() if k != "calibration"}, indent=1))
