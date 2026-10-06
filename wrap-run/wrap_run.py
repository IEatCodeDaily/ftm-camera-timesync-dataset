"""Long wrap-crossing run using only the camera nodes (no logic analyzer).

All nodes run FREX-triggered (sensor exposure started by the shared-clock trigger) QVGA capture at\n10 frames/s, the 100 %-yield profile. FREX matters: without it the sensor free-runs and capture\ninstants measure sensor phase, not the node clocks.
Every frame carries the trigger tick it answered and its capture instant, both in
hub time. Per tick, spread = max - min capture across nodes. A wrap-handling fault
(FTM 48-bit ps wrap every 281.47 s, MAC 32-bit us wrap every ~71.6 min) shows up as
a step in spread, a missing node, or sync_valid dropping - the known faults were
50-475 ms jumps, ~1000x above this method's ~tens-of-us resolution.

Usage: [NODES=0,3] python3 wrap_run.py <seconds> <out.jsonl> ["node cmd; node cmd"]
The optional third argument is sent to every node once capture runs, e.g.
"cam hwstamp on; cam hwtrig on" for the hardware capture-timing A/B.
"""
import json, struct, sys, time, urllib.request

BASE, HDRS, REC_HDR = "http://127.0.0.1:3001", {"Content-Type": "application/json", "x-mocap-studio": "1"}, 29
dur, out = float(sys.argv[1]), open(sys.argv[2], "a")
extra = [c.strip() for c in (sys.argv[3] if len(sys.argv) > 3 else "").split(";") if c.strip()]

def cmd(addr, text):
    req = urllib.request.Request(f"{BASE}/api/nodes/{addr}/command", json.dumps({"cmd": text}).encode(), HDRS, method="POST")
    try: return urllib.request.urlopen(req, timeout=90).read().decode().strip()
    except Exception as e: return f"ERR {e}"

def nodes():
    return json.load(urllib.request.urlopen(f"{BASE}/api/nodes", timeout=30))

def snapshot():
    blob = urllib.request.urlopen(urllib.request.Request(f"{BASE}/api/camera/snapshot", headers=HDRS), timeout=15).read()
    recs, off = [], 0
    while off + REC_HDR <= len(blob):
        node = blob[off]; tick, cap = struct.unpack_from("<QQ", blob, off + 9); n = struct.unpack_from("<I", blob, off + 25)[0]
        recs.append((node, tick, cap)); off += REC_HDR + n
    return recs

def log(kind, **k): out.write(json.dumps({"t": time.time(), "kind": kind, **k}) + "\n"); out.flush()

ns = {n["node_id"]: n["ip_address"] for n in nodes() if n.get("online") and n.get("ip_address")}
import os
if os.environ.get("NODES"): ns = {k: v for k, v in ns.items() if str(k) in os.environ["NODES"].split(",")}
log("start", nodes=ns, dur=dur)
for a in ns.values(): cmd(a, "mode off")
time.sleep(14)
for a in ns.values(): cmd(a, "collector 192.168.137.1"); log("frex", addr=a, reply=cmd(a, "cam syncfrex on")[:200])
for a in ns.values(): log("mode", addr=a, reply=cmd(a, "mode udp 10 qvga sync --nomesh")[:200])
time.sleep(22)
for a in ns.values():
    for c in extra: log("extra", addr=a, cmd=c, reply=cmd(a, c)[:200])
for a in ns.values():
    for c in ("cam hwstamp stats", "cam hwtrig stats"): cmd(a, c)   # reset counters at t0
for a in ns.values():
    log("stats0", addr=a, cmd="sync", reply=" ".join(l for l in cmd(a, "cam stats").splitlines() if l.startswith("sync:")))

t0, seen, last_health = time.time(), set(), 0.0
try:
    while time.time() - t0 < dur:
        try:
            for node, tick, cap in snapshot():
                if (node, tick) not in seen:
                    seen.add((node, tick)); log("f", node=node, tick=tick, cap=cap)
        except Exception as e: log("err", e=str(e)[:200])
        if time.time() - last_health > 5:
            last_health = time.time()
            try: log("health", n=[{k: n.get(k) for k in ("node_id", "role", "online", "sync_valid", "drift_ppm", "resid_ns_std", "sync_age_ms", "camera_running")} for n in nodes()])
            except Exception as e: log("err", e=str(e)[:200])
        time.sleep(0.04)
finally:
    for a in ns.values():
        for c in ("cam hwstamp stats", "cam hwtrig stats"): log("stats", addr=a, cmd=c, reply=cmd(a, c)[:400])
        # node-side send counter: frames lost between node and logger vs never sent
        log("stats", addr=a, cmd="sync", reply=" ".join(l for l in cmd(a, "cam stats").splitlines() if l.startswith("sync:")))
    for a in ns.values(): cmd(a, "mode off")
    log("end", frames=len(seen))
