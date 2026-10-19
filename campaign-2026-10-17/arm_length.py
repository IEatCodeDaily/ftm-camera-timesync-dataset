"""Length accuracy against the Dobot (2026-10-19). The arm's own encoders give the tool point; the
tracker gives the marker's tool point. Distance between two reached poses is a reference length that
never passes through the tracker's scale or registration. Wide grid over the arm's reach, one visit
each, settle then 1 s median. Pixel-bias learning off. Read-only for exposure/gain/rig calibration.
Usage: python arm_length.py <out.json>"""
import json, sys, time, math, urllib.request, statistics as st
API = "http://127.0.0.1:3001/api"; H = {"x-mocap-studio": "1", "Content-Type": "application/json"}
def req(p, b=None):
    out = urllib.request.urlopen(urllib.request.Request(API + p, None if b is None else json.dumps(b).encode(), H), timeout=20).read()
    return json.loads(out or b"null") if b is None else out
def wait_still(timeout=20.0):
    t0 = time.time(); last = None
    while time.time() - t0 < timeout:
        s = req("/arm"); p = s["pose"]["xyz"] if s.get("pose") else None
        if s["mode"] == "idle" and p and last and max(abs(a - b) for a, b in zip(p, last)) < 0.05: return s
        last = p; time.sleep(0.4)
    raise TimeoutError
def sample(cmd):
    time.sleep(0.4); ir, pose = [], None; t_end = time.time() + 1.0
    while time.time() < t_end:
        s = req("/arm")
        if s.get("ee_seen") and s.get("ee_ir"): ir.append(s["ee_ir"]); pose = s["pose"]
        time.sleep(0.05)
    if len(ir) < 5: return {"cmd": cmd, "error": f"ir samples {len(ir)}"}
    return {"cmd": cmd, "pose_xyz": pose["xyz"], "joints": pose["joints"], "n": len(ir),
            "ir_world": [st.median(c) for c in zip(*ir)], "ir_sd": [st.pstdev(c) for c in zip(*ir)]}
def plan():
    P = []
    for z in (-40, 20, 80, 130):
        for r in (170, 230, 290):
            for a in range(-75, 76, 25):
                P.append([round(r * math.cos(math.radians(a)), 1), round(r * math.sin(math.radians(a)), 1), z])
    return P
if __name__ == "__main__":
    s = req("/arm"); assert s["connected"] and s["homed"] and s.get("calibration")
    start = s["pose"]["xyz"]; rows = []
    for i, p in enumerate(plan()):
        try: req("/arm/move", {"xyz": p}); wait_still()
        except Exception as e: rows.append({"cmd": p, "error": str(e)}); continue
        r = sample(p); rows.append(r)
        print(i, p, "reached" if "pose_xyz" in r and math.dist(r["pose_xyz"], p) < 1 else "SNAP/ERR", r.get("n"), flush=True)
    req("/arm/move", {"xyz": start}); wait_still()
    json.dump({"calibration": s["calibration"], "speed_pct": s["config"]["speed_pct"], "rows": rows}, open(sys.argv[1], "w"), indent=1)
    print("wrote", len(rows))
