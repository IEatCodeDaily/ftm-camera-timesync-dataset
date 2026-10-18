"""Post-restart settling (2026-10-19): does the tracked world move after a tracking
restart, and is the per-camera pixel-bias learning the cause? For each condition,
restart tracking (user's tracking_start.json knobs, 35 fps), then log for SECONDS:
the static Calibrator centroid, the parked arm marker (ee_ir) and the learned
pixel_bias. Conditions: bias on (default rate), bias off (tracking.json
bias_learn_rate 0), bias on again. Read-only for exposure, gain, calibration.
Usage: python restart_settle.py <out.json> [seconds]"""
import json, sys, time, threading, urllib.request, websocket, os
API = "http://127.0.0.1:3001/api"; H = {"x-mocap-studio": "1", "Content-Type": "application/json"}
STUDIO = r"E:\Projects\wireless-ir-mocap\mocap\studio"
def req(p, b=None):
    out = urllib.request.urlopen(urllib.request.Request(API + p, None if b is None else json.dumps(b).encode(), H), timeout=60).read()
    return json.loads(out or b"null") if b is None else out
latest = {}
def ws_loop():
    while True:
        try:
            ws = websocket.create_connection("ws://127.0.0.1:3001/ws", timeout=5)
            while True:
                d = json.loads(ws.recv())
                if d.get("event") == "pose3d": latest["p"] = d["payload"]; latest["t"] = time.time()
        except Exception: time.sleep(1)
threading.Thread(target=ws_loop, daemon=True).start()
def calibrator():
    p = latest.get("p") or {}
    b = next((b for b in p.get("bodies", []) if b["name"] == "Calibrator"), None)
    if not b: return None
    pos = {t["id"]: t["position"] for t in p.get("tracks", [])}
    pts = [pos.get(i) for i in b.get("marker_track_ids", [])]
    return pts if len(pts) == 3 and all(pts) else None
def set_rate(rate):
    path = os.path.join(STUDIO, "tracking.json")
    if rate is None:
        if os.path.exists(path): os.remove(path)
    else: json.dump({"bias_learn_rate": rate}, open(path, "w"))
def run(label, rate, seconds):
    set_rate(rate)
    req("/tracking/stop", {}); time.sleep(3)
    cfg = json.load(open(os.path.join(STUDIO, "tracking_start.json"))); cfg["target_node_ids"] = [0, 1, 2, 3]; cfg["fps"] = 35
    req("/tracking/start", cfg); t0 = time.time()
    for ip in ("192.168.137.172", "192.168.137.197", "192.168.137.230", "192.168.137.31"):
        req(f"/nodes/{ip}/command", {"cmd": "cam hwstamp on"}); req(f"/nodes/{ip}/command", {"cmd": "cam hwtrig on"})
    rows = []
    while time.time() - t0 < seconds:
        c = calibrator(); a = req("/arm"); st = req("/tracking/status")
        rows.append({"t": round(time.time() - t0, 2), "cal": c, "ee": a.get("ee_ir") if a.get("ee_seen") else None,
                     "bias": st.get("pixel_bias") or (st.get("detect") or {}).get("pixel_bias")})
        time.sleep(1.0)
    print(label, "rows", len(rows), "cal ok", sum(r["cal"] is not None for r in rows), "ee ok", sum(r["ee"] is not None for r in rows), flush=True)
    return {"label": label, "bias_learn_rate": rate, "rows": rows}
if __name__ == "__main__":
    out = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 240
    a = req("/arm"); assert a["connected"] and a["homed"]
    time.sleep(2)
    res = {"arm_pose": a["pose"]["xyz"], "runs": []}
    try:
        for label, rate in (("bias_on", None), ("bias_off", 0.0), ("bias_on_2", None)):
            res["runs"].append(run(label, rate, secs))
            json.dump(res, open(out, "w"), indent=0)
    finally:
        set_rate(None)
    print("wrote", out)
