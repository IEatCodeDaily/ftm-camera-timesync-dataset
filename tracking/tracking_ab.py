"""Tracking-mode A/B of the capture-path hardware timing (cam hwstamp / cam hwtrig).

Starts Studio live tracking (VGA, 35 fps, all nodes) twice, once per setting,
each for DUR seconds; Studio records each session to an MCAP. Then compares the
per-slot spread of capture_us across cameras, exactly as tracking/analyze_tracking.py
section B does, and the per-camera trigger->capture delay.

Usage: python3 tracking_ab.py <seconds> <outdir>
"""
import json, os, shutil, sys, time, urllib.request
from collections import defaultdict
import numpy as np
from mcap.reader import make_reader

B, H = "http://127.0.0.1:3001", {"Content-Type": "application/json", "x-mocap-studio": "1"}
DUR, OUT = float(sys.argv[1]), sys.argv[2]
FPS = 35
os.makedirs(OUT, exist_ok=True)


def req(path, body=None, method="GET"):
    r = urllib.request.Request(B + path, json.dumps(body).encode() if body is not None else None, H, method=method)
    return urllib.request.urlopen(r, timeout=120).read().decode()


def node_cmd(ip, text):
    try: return req(f"/api/nodes/{ip}/command", {"cmd": text}, "POST")
    except Exception as e: return f"ERR {e}"


def session(tag, on):
    ns = {n["node_id"]: n["ip_address"] for n in json.loads(req("/api/nodes")) if n.get("online")}
    for ip in ns.values():
        for c in (f"cam hwstamp {on}", f"cam hwtrig {on}"): node_cmd(ip, c)
    req("/api/tracking/start", {"target_node_ids": sorted(ns), "resolution": "vga", "fps": FPS}, "POST")
    time.sleep(20)                                       # start-up + heal window
    for ip in ns.values():
        for c in ("cam hwstamp stats", "cam hwtrig stats"): node_cmd(ip, c)   # reset counters
    t0 = time.time()
    time.sleep(DUR)
    st = {ip: [node_cmd(ip, c).splitlines()[-1] for c in ("cam hwstamp stats", "cam hwtrig stats")] for ip in ns.values()}
    status = json.loads(req("/api/tracking/status"))
    req("/api/tracking/stop", {}, "POST")
    time.sleep(5)
    src = status["recording"]["path"].replace("E:\\", "/mnt/e/").replace("\\", "/")
    dst = os.path.join(OUT, f"tracking-{tag}.mcap")
    shutil.copy(src, dst)
    json.dump({"tag": tag, "hw": on, "t0": t0, "dur": DUR, "node_stats": st, "status": status},
              open(os.path.join(OUT, f"tracking-{tag}.json"), "w"), indent=1)
    return dst, t0


def analyse(path, t0, dur):
    rows = []
    with open(path, "rb") as fh:
        for _, _, m in make_reader(fh).iter_messages(topics=["/mocap/centroids"]):
            rows.append(json.loads(m.data))
    period = 1e6 / FPS
    # host_time_ns = arrival; keep the steady window only (counters were reset at t0)
    rows = [r for r in rows if t0 * 1e9 <= r["host_time_ns"] <= (t0 + dur) * 1e9]
    slots = defaultdict(dict)
    for r in rows:
        k = round(r["capture_us"] / period)
        slots[k][r["camera_id"]] = r["capture_us"] - k * period
    cams = sorted({c for s in slots.values() for c in s})
    full = [s for s in slots.values() if len(s) == len(cams)]
    spread = np.array([max(s.values()) - min(s.values()) for s in full])
    med = {c: float(np.median([s[c] for s in slots.values() if c in s])) for c in cams}
    jit = np.array([max(v - med[c] for c, v in s.items()) - min(v - med[c] for c, v in s.items()) for s in full])
    q = lambda a: {"median": float(np.median(a)), "p95": float(np.percentile(a, 95)),
                   "p99": float(np.percentile(a, 99)), "max": float(a.max())}
    return {"cameras": cams, "packets": len(rows), "slots": len(slots), "full_slots": len(full),
            "full_yield": len(full) / max(1, round(dur * FPS)),
            "per_camera_fps": {c: sum(c in s for s in slots.values()) / dur for c in cams},
            "spread_us": q(spread), "jitter_fixed_removed_us": q(jit),
            "per_camera_offset_from_slot_us": med}


if __name__ == "__main__":
    res = {}
    for tag, on in (("sw", "off"), ("hw", "on")):
        path, t0 = session(tag, on)
        res[tag] = analyse(path, t0, DUR)
        print(tag, json.dumps(res[tag], indent=1), flush=True)
    json.dump(res, open(os.path.join(OUT, "tracking_ab.stats.json"), "w"), indent=1)
