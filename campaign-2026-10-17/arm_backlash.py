"""Backlash vs tracker drift (2026-10-18). Same 18 held-out poses and settle/sample
rule as arm-reference/arm_reference.py. Two phases, both 2 visits per pose:
  A same-side: visit pose i twice, each time from a fixed pre-point (-20 mm in x),
    so only the return error with an identical approach remains.
  B opposite-side: plan forward then reversed (the campaign's arm_reference order).
At every sample the static calibrator's three tracks are logged: a drift of the
tracker or its registration moves them, arm backlash does not.
Read-only for calibration, exposure, gain. Usage: python arm_backlash.py <out.json>"""
import json, sys, time, math, urllib.request, statistics as st
import websocket, threading

API = "http://127.0.0.1:3001/api"
H = {"x-mocap-studio": "1", "Content-Type": "application/json"}


def req(path, body=None):
    r = urllib.request.Request(API + path, None if body is None else json.dumps(body).encode(), H, method="GET" if body is None else "POST")
    out = urllib.request.urlopen(r, timeout=20).read()
    return json.loads(out or b"null") if body is None else out


latest = {}
def ws_loop():
    ws = websocket.create_connection("ws://127.0.0.1:3001/ws", timeout=5)
    while True:
        try: d = json.loads(ws.recv())
        except Exception: continue
        if d.get("event") == "pose3d": latest["tracks"] = d["payload"].get("tracks", []); latest["bodies"] = d["payload"].get("bodies", [])
threading.Thread(target=ws_loop, daemon=True).start()


def calibrator():
    """The static Calibrator body's three LED tracks (Studio's body fit picks them), world mm."""
    body = next((b for b in latest.get("bodies", []) if b["name"] == "Calibrator"), None)
    if not body: return [None, None, None]
    pos = {t["id"]: t["position"] for t in latest.get("tracks", [])}
    return [pos.get(i) for i in body.get("marker_track_ids", [])][:3] + [None] * (3 - len(body.get("marker_track_ids", [])))


def wait_still(timeout=20.0):
    t0 = time.time(); last = None
    while time.time() - t0 < timeout:
        s = req("/arm"); p = s["pose"]["xyz"] if s.get("pose") else None
        if s["mode"] == "idle" and p and last and max(abs(a - b) for a, b in zip(p, last)) < 0.05: return s
        last = p; time.sleep(0.5)
    raise TimeoutError


def move(p): req("/arm/move", {"xyz": p}); wait_still()


def sample(p, phase, visit):
    time.sleep(0.3)
    ir, rw, cal, pose = [], [], [], None
    t_end = time.time() + 1.0
    while time.time() < t_end:
        s = req("/arm")
        if s.get("ee_seen") and s.get("ee_ir") and s.get("ee_reported_world"):
            ir.append(s["ee_ir"]); rw.append(s["ee_reported_world"]); pose = s["pose"]
        c = calibrator()
        if all(c): cal.append(c)
        time.sleep(0.05)
    if len(ir) < 5: return {"cmd": p, "phase": phase, "visit": visit, "error": f"ir samples {len(ir)}"}
    med = lambda rows: [st.median(c) for c in zip(*rows)]
    row = {"cmd": p, "phase": phase, "visit": visit, "pose_xyz": pose["xyz"], "joints": pose["joints"], "n": len(ir),
           "ir_world": med(ir), "reported_world": med(rw), "ir_sd": [st.pstdev(c) for c in zip(*ir)], "t": time.time(),
           "calibrator": [med([c[k] for c in cal]) for k in range(3)] if len(cal) >= 5 else None, "cal_n": len(cal)}
    row["err"] = [a - b for a, b in zip(row["ir_world"], row["reported_world"])]
    print(f"{phase} v{visit} {p} |err| {math.hypot(*row['err']):5.2f} cal_n {len(cal)}", flush=True)
    return row


def plan():
    return [[round(r * math.cos(math.radians(a)), 1), round(r * math.sin(math.radians(a)), 1), z]
            for z in (15, 50, 85) for r in (200, 250) for a in (-20, 0, 20)]


if __name__ == "__main__":
    out = sys.argv[1]
    s = req("/arm")
    assert s["connected"] and s["homed"] and s.get("calibration") and s["config"]["ee_body"] == s["calibration"]["body"]
    time.sleep(2); assert all(calibrator()), f"calibrator not tracked: {calibrator()}"
    start = s["pose"]["xyz"]; rows = []; P = plan()
    for i, p in enumerate(P):                                   # A: same approach both visits
        for v in range(2):
            move([p[0] - 20, p[1], p[2]]); move(p); rows.append(sample(p, "A", v))
    for v, order in enumerate((P, P[::-1])):                    # B: opposite approach (campaign order)
        for p in order: move(p); rows.append(sample(p, "B", v))
    move(start)
    json.dump({"start": start, "speed_pct": s["config"]["speed_pct"], "calibration": s["calibration"], "rows": rows}, open(out, "w"), indent=1)
    print("wrote", out, len(rows), "rows")
