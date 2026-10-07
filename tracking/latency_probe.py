"""Latency decomposition of the live tracking path (paper: recommendations).

1. fps sweep: start Studio tracking at several paced rates for a short window
   each and record per-packet frame_ready_us (capture VSYNC -> frame handed to
   the detector loop), detection_us, preprocess_us. If frame_ready tracks the
   slot period, the loop's pipelining (grab at the NEXT slot) sets it; if it
   stays flat, sensor readout does.
2. Wi-Fi delivery: host arrival minus hub capture time per packet; the
   hub->host clock offset is unknown, so delivery is reported as excess over
   its own minimum, and the minimum is bounded by the ICMP round trip.
3. Host grouping: per slot, last camera's arrival minus first's = time the
   estimator waits before it can close the group.

Usage: python3 latency_probe.py <seconds_per_rate> <outdir> [fps ...]
"""
import json, os, shutil, subprocess, sys, time, urllib.request
from collections import defaultdict
import numpy as np
from mcap.reader import make_reader

B, H = "http://127.0.0.1:3001", {"Content-Type": "application/json", "x-mocap-studio": "1"}
DUR, OUT = float(sys.argv[1]), sys.argv[2]
RATES = [int(x) for x in sys.argv[3:]] or [35, 20, 10]
os.makedirs(OUT, exist_ok=True)


def req(path, body=None, method="GET"):
    r = urllib.request.Request(B + path, json.dumps(body).encode() if body is not None else None, H, method=method)
    return urllib.request.urlopen(r, timeout=120).read().decode()


def q(a):
    a = np.asarray(a, float)
    return {"p5": float(np.percentile(a, 5)), "median": float(np.median(a)), "p95": float(np.percentile(a, 95)),
            "p99": float(np.percentile(a, 99)), "max": float(a.max()), "n": int(a.size)}


def run(fps):
    ns = {n["node_id"]: n["ip_address"] for n in json.loads(req("/api/nodes")) if n.get("online")}
    for ip in ns.values():
        for c in ("cam hwstamp on", "cam hwtrig on"):
            for attempt in range(3):          # a node command occasionally 400s transiently
                try: req(f"/api/nodes/{ip}/command", {"cmd": c}, "POST"); break
                except Exception:
                    if attempt == 2: raise
                    time.sleep(2)
    req("/api/tracking/start", {"target_node_ids": sorted(ns), "resolution": "vga", "fps": fps}, "POST")
    time.sleep(15)
    t0 = time.time(); time.sleep(DUR)
    status = json.loads(req("/api/tracking/status"))
    req("/api/tracking/stop", {}, "POST"); time.sleep(5)
    src = status["recording"]["path"].replace("E:\\", "/mnt/e/").replace("\\", "/")
    dst = os.path.join(OUT, f"latency-{fps}fps.mcap"); shutil.copy(src, dst)
    json.dump({"t0": t0, "dur": DUR, "nodes": len(ns)}, open(dst + ".json", "w"))
    return analyse(dst, fps, t0, DUR, len(ns))


def analyse(dst, fps, t0, dur, n_nodes):
    rows = [json.loads(m.data) for _, _, m in make_reader(open(dst, "rb")).iter_messages(topics=["/mocap/centroids"])]
    rows = [r for r in rows if t0 * 1e9 <= r["host_time_ns"] <= (t0 + dur) * 1e9]
    period = 1e6 / fps
    deliv = defaultdict(list); slots = defaultdict(list)
    for r in rows:
        onnode = r["frame_ready_us"] + r["preprocess_us"] + r["detection_us"]
        d = r["host_time_ns"] / 1e3 - r["capture_us"] - onnode       # delivery + unknown offset
        deliv[r["camera_id"]].append(d)
        slots[round(r["capture_us"] / period)].append(r["host_time_ns"] / 1e3)
    base = min(min(v) for v in deliv.values())                       # common offset: hub clock is shared
    # Per slot: frame-start (capture_us) -> LAST camera's packet at the host, i.e. when the
    # estimator can first close the group. Reported up to the unknown fastest one-way Wi-Fi
    # time (>= 0, bounded by ICMP RTT): last_arrival - capture - base + onnode_of_that_packet.
    full = defaultdict(list)
    for r in rows:
        full[round(r["capture_us"] / period)].append(r["host_time_ns"] / 1e3 - r["capture_us"])
    slot_ready = [max(v) - base for v in full.values() if len(v) == n_nodes]
    return {"fps": fps, "slot_period_us": period, "packets": len(rows),
            "frame_ready_us": q([r["frame_ready_us"] for r in rows]),
            "detection_us": q([r["detection_us"] for r in rows]),
            "preprocess_us": q([r["preprocess_us"] for r in rows]),
            "delivery_excess_us": q([x - base for v in deliv.values() for x in v]),
            "slot_arrival_spread_us": q([max(v) - min(v) for v in slots.values() if len(v) == n_nodes]),
            "send_after_capture_us": q([r["frame_ready_us"] + r["preprocess_us"] + r["detection_us"] for r in rows]),
            "capture_to_last_camera_at_host_us": q(slot_ready)}


def ping(ip, n=50):
    out = subprocess.run(["/mnt/c/Windows/System32/PING.EXE", "-n", str(n), ip], capture_output=True, text=True).stdout
    import re
    t = [float(x) for x in re.findall(r"time[=<]([\d.]+)ms", out)]
    return q(t) if t else None


if __name__ == "__main__":
    if os.environ.get("ANALYSE_ONLY"):   # re-analyse saved recordings (t0 from the sidecar json)
        res = {"rates": []}
        for f in RATES:
            dst = os.path.join(OUT, f"latency-{f}fps.mcap"); m = json.load(open(dst + ".json"))
            res["rates"].append(analyse(dst, f, m["t0"], m["dur"], m["nodes"]))
    else:
        res = {"rates": [run(f) for f in RATES]}
    if os.environ.get("ANALYSE_ONLY"):
        res["icmp_rtt_ms"] = json.load(open(os.path.join(OUT, "latency.stats.json")))["icmp_rtt_ms"]
    else:
        ns = {n["node_id"]: n["ip_address"] for n in json.loads(req("/api/nodes")) if n.get("online")}
        res["icmp_rtt_ms"] = {k: ping(ip) for k, ip in ns.items()}
    json.dump(res, open(os.path.join(OUT, "latency.stats.json"), "w"), indent=1)
    for r in res["rates"]:
        print(r["fps"], "frame_ready med", r["frame_ready_us"]["median"], "det med", r["detection_us"]["median"],
              "deliv excess med/p95/p99", r["delivery_excess_us"]["median"], r["delivery_excess_us"]["p95"], r["delivery_excess_us"]["p99"],
              "slot spread med/p95", r["slot_arrival_spread_us"]["median"], r["slot_arrival_spread_us"]["p95"],
              "capture->last cam at host med/p95/p99", r["capture_to_last_camera_at_host_us"]["median"],
              r["capture_to_last_camera_at_host_us"]["p95"], r["capture_to_last_camera_at_host_us"]["p99"])
    print("icmp", {k: (v["p5"], v["median"]) for k, v in res["icmp_rtt_ms"].items() if v})
