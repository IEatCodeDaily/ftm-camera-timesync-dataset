"""Optimization ablation on the live rig: same scene, same nodes, one config per trial.

Each trial: stop tracking -> set every knob explicitly on every node -> start paced VGA
tracking -> settle -> measure DUR s -> read `cam stats` -> stop -> copy MCAP -> analyse.
Latency analysis = dataset tracking/latency_probe.py analyse() (host-vs-hub drift removed
by a line through per-10-s minima; capture->last camera at host is a lower bound).

usage: python ablation.py <outdir> <seconds> <trial-name>... (names from TRIALS)
"""
import json, os, re, shutil, sys, time, urllib.request
from collections import defaultdict
import numpy as np
from mcap.reader import make_reader

B, H = "http://127.0.0.1:3001", {"Content-Type": "application/json", "x-mocap-studio": "1"}
# Fixed scene control: threshold 180 / min_area 2 gives the static rig ~1-2 markers per camera per frame
# (scene_probe.py). Without it the nodes keep whatever threshold was last set, and an earlier
# low-threshold run made 15-30 blobs/frame and saturated the 31-marker cap -> unequal detector load.
THRESH = int(__import__("os").environ.get("THRESH", "180"))
MIN_AREA = int(__import__("os").environ.get("MIN_AREA", "2"))

# knob order matters only for readability; every trial sets all of them.
BASE = {"algo": 3, "rowstream": "on", "syncbufs": 2, "hwtrig": "on", "hwstamp": "on", "lowlat": "off", "vskip": 2, "fps": 35}
TRIALS = {
    # cumulative ladder: naive -> shipped
    "L0_naive":     dict(BASE, algo=0, rowstream="off", syncbufs=1, hwtrig="off", hwstamp="off"),
    "L1_simd":      dict(BASE, algo=3, rowstream="off", syncbufs=1, hwtrig="off", hwstamp="off"),
    "L2_syncbufs":  dict(BASE, algo=3, rowstream="off", syncbufs=2, hwtrig="off", hwstamp="off"),
    "L3_rowstream": dict(BASE, algo=3, rowstream="on",  syncbufs=2, hwtrig="off", hwstamp="off"),
    "L4_hwtiming":  dict(BASE),                                   # shipped configuration
    # detector algorithms on identical capture path (full-frame detect after readout)
    "D0_scalar":    dict(BASE, algo=0, rowstream="off"),
    "D1_swar":      dict(BASE, algo=1, rowstream="off"),
    "D2_pieprefix": dict(BASE, algo=2, rowstream="off"),
    "D3_piefull":   dict(BASE, algo=3, rowstream="off"),
    # low-latency grab (only valid <= 30 fps)
    "W30_default":  dict(BASE, fps=30),
    "W30_lowlat":   dict(BASE, fps=30, lowlat="on"),
    # single-buffer configs collapse at 35 (half-slot grab timeout -> capture_fail -> heal loop, and
    # bufs=1 frames land a slot apart); measure them at a rate they sustain instead.
    "L0_naive10":   dict(BASE, fps=10, algo=0, rowstream="off", syncbufs=1, hwtrig="off", hwstamp="off"),
    "L1_simd20":    dict(BASE, fps=20, algo=3, rowstream="off", syncbufs=1, hwtrig="off", hwstamp="off"),
    # pipelining cost at a low rate: shipped config vs low-latency grab, both at 10 fps
    "W10_default":  dict(BASE, fps=10),
    "W10_lowlat":   dict(BASE, fps=10, lowlat="on"),
    # 2026-10 firmware: same-slot send needs the 12.4 ms readout of 4x vertical sampling
    "F0_vskip2":    dict(BASE),                                   # = L4 on the new build
    "F1_vskip4":    dict(BASE, vskip=4),                          # half readout, pipelined grab
    "F2_fast":      dict(BASE, vskip=4, lowlat="on"),             # shipped 2026-10 default
    "F2_fast30":    dict(BASE, vskip=4, lowlat="on", fps=30),
    "F2_fast20":    dict(BASE, vskip=4, lowlat="on", fps=20),
    "F0_vskip2_20": dict(BASE, fps=20),
}


def req(path, body=None, method="GET", timeout=120):
    r = urllib.request.Request(B + path, json.dumps(body).encode() if body is not None else None, H, method=method)
    return urllib.request.urlopen(r, timeout=timeout).read().decode()


def cmd(ip, c):
    for attempt in range(4):
        try:
            return req(f"/api/nodes/{ip}/command", {"cmd": c}, "POST")
        except Exception as e:
            if attempt == 3:
                raise RuntimeError(f"{ip}: {c}: {e}")
            time.sleep(2)


def nodes():
    return {n["node_id"]: n["ip_address"] for n in json.loads(req("/api/nodes")) if n.get("online") and n.get("ip_address")}


def q(a):
    a = np.asarray(a, float)
    if a.size == 0:
        return None
    return {"p5": float(np.percentile(a, 5)), "median": float(np.median(a)), "mean": float(a.mean()),
            "p95": float(np.percentile(a, 95)), "p99": float(np.percentile(a, 99)), "max": float(a.max()), "n": int(a.size)}


def analyse(path, fps, t0, dur, n_nodes):
    rows = [json.loads(m.data) for _, _, m in make_reader(open(path, "rb")).iter_messages(topics=["/mocap/centroids"])]
    rows = [r for r in rows if t0 * 1e9 <= r["host_time_ns"] <= (t0 + dur) * 1e9]
    period = 1e6 / fps
    cap = np.array([r["capture_us"] for r in rows], float)
    raw = np.array([r["host_time_ns"] / 1e3 - r["capture_us"] - (r["frame_ready_us"] + r["preprocess_us"] + r["detection_us"]) for r in rows])
    tb = ((cap - cap.min()) // 10e6).astype(int)
    xs = np.array([cap[tb == b].mean() for b in np.unique(tb)]); ys = np.array([raw[tb == b].min() for b in np.unique(tb)])
    slope, icpt = np.polyfit(xs, ys, 1) if len(xs) > 1 else (0.0, ys[0])
    env = slope * cap + icpt
    deliv = raw - env
    base = deliv.min()
    full = defaultdict(list)
    for r, e in zip(rows, env):
        full[round(r["capture_us"] / period)].append(r["host_time_ns"] / 1e3 - r["capture_us"] - e)
    slot_ready = [max(v) - base for v in full.values() if len(v) == n_nodes]
    per_cam = defaultdict(list)
    for r in rows:
        per_cam[r["camera_id"]].append(r["capture_us"])
    fps_cam = {str(k): (len(v) - 1) / ((max(v) - min(v)) / 1e6) for k, v in per_cam.items() if len(v) > 1}
    markers = [len(r.get("markers", [])) for r in rows]
    return {"fps_target": fps, "packets": len(rows), "full_slots": len(slot_ready), "host_vs_hub_drift_ppm": float(slope * 1e6),
            "per_camera_fps": fps_cam, "markers_per_packet": float(np.mean(markers)) if markers else 0.0,
            "frame_ready_us": q([r["frame_ready_us"] for r in rows]),
            "detection_us": q([r["detection_us"] for r in rows]),
            "preprocess_us": q([r["preprocess_us"] for r in rows]),
            "send_after_capture_us": q([r["frame_ready_us"] + r["preprocess_us"] + r["detection_us"] for r in rows]),
            "delivery_excess_us": q(deliv - base),
            "capture_to_last_camera_at_host_us": q(slot_ready)}


STAT_KEYS = ("track: ", "track source:", "track timing us:", "track age us:", "track rowstream:", "camera profile:")


def stats(ip):
    out = cmd(ip, "cam stats")
    return [l for l in out.splitlines() if l.startswith(STAT_KEYS)]


def run(name, cfg, out, dur):
    ns = nodes()
    try:
        req("/api/tracking/stop", {}, "POST")
    except Exception:
        pass
    time.sleep(4)
    applied = {}
    for nid, ip in sorted(ns.items()):
        replies = []
        for c in (f"cam algo {cfg['algo']}", f"cam rowstream {cfg['rowstream']}", f"cam syncbufs {cfg['syncbufs']}",
                  f"cam hwtrig {cfg['hwtrig']}", f"cam hwstamp {cfg['hwstamp']}", f"cam lowlat {cfg['lowlat']}", f"cam vskip {cfg['vskip']}"):
            r = cmd(ip, c)
            if "OK" not in r:
                raise RuntimeError(f"node {nid}: `{c}` -> {r.strip()[-200:]}")
            replies.append(r.strip().splitlines()[-1])
        applied[nid] = replies
    for attempt in range(3):
        try:
            req("/api/tracking/start", {"target_node_ids": sorted(ns), "resolution": "vga", "fps": cfg["fps"],
                                        "threshold": THRESH, "min_area": MIN_AREA}, "POST", 180)
            break
        except Exception:
            if attempt == 2:
                raise
            req("/api/tracking/stop", {}, "POST"); time.sleep(10)
    time.sleep(12)
    for ip in ns.values():                       # reset hw counters so stats cover the window
        cmd(ip, "cam hwtrig stats"); cmd(ip, "cam hwstamp stats")
    before = {nid: stats(ip) for nid, ip in ns.items()}
    t0 = time.time(); time.sleep(dur)
    after = {nid: stats(ip) for nid, ip in ns.items()}
    hw = {nid: [cmd(ip, "cam hwtrig stats").strip().splitlines()[-1], cmd(ip, "cam hwstamp stats").strip().splitlines()[-1]] for nid, ip in ns.items()}
    status = json.loads(req("/api/tracking/status"))
    req("/api/tracking/stop", {}, "POST"); time.sleep(5)
    src = status["recording"]["path"].replace("E:\\", "/mnt/e/").replace("\\", "/")
    dst = os.path.join(out, f"{name}.mcap"); shutil.copy(src, dst)
    res = {"name": name, "config": cfg, "t0": t0, "dur": dur, "nodes": sorted(ns), "applied": applied,
           "stats_before": before, "stats_after": after, "hw_stats": hw, "detect_status": status.get("detect"),
           "analysis": analyse(dst, cfg["fps"], t0, dur, len(ns))}
    json.dump(res, open(os.path.join(out, f"{name}.json"), "w"), indent=1)
    a = res["analysis"]
    f = lambda k, s=1e3: (a[k] or {}).get("median", float("nan")) / s
    print(f"{name}: fps={ {k: round(v, 1) for k, v in a['per_camera_fps'].items()} } full_slots={a['full_slots']} "
          f"det_med={f('detection_us', 1):.0f}us send_med={f('send_after_capture_us'):.1f}ms "
          f"e2e_med={f('capture_to_last_camera_at_host_us'):.1f}ms mk/pkt={a['markers_per_packet']:.2f}", flush=True)


if __name__ == "__main__":
    out, dur = sys.argv[1], float(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    for name in sys.argv[3:]:
        try:
            run(name, TRIALS[name], out, dur)
        except Exception as e:
            print(f"{name}: FAILED {e}", flush=True)
            json.dump({"name": name, "error": str(e)}, open(os.path.join(out, f"{name}.error.json"), "w"))
    try:
        req("/api/tracking/stop", {}, "POST")
    except Exception:
        pass
    # leave the fleet on the shipped configuration
    for ip in nodes().values():
        for c in ("cam algo 3", "cam rowstream on", "cam syncbufs 2", "cam hwtrig on", "cam hwstamp on", "cam lowlat on", "cam vskip 4"):
            try: cmd(ip, c)
            except Exception: pass
    print("done", flush=True)
