"""FPS ceiling probe: read OV5640 timing registers while tracking, then try target rates and report achieved rate.

Usage (Windows python, to avoid WSL->Studio stalls): py -3.11 fps_ceiling.py OUTDIR 35 38 40 45 50
Each rate: shipped config (hwtrig, hwstamp, lowlat on, vskip 4), operator knobs, 15 s settle + DUR s recording.
Reports per-camera fps, full-slot yield and per-packet latency via latency_probe.analyse().
"""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("LOWLAT", "on"); os.environ.setdefault("VSKIP", "4")
if os.name == "nt": os.environ.setdefault("KNOBS", r"E:\Projects\wireless-ir-mocap\mocap\studio\tracking_start.json")
OUT, RATES = sys.argv[1], [int(x) for x in sys.argv[2:]]
sys.argv = [sys.argv[0], os.environ.get("DUR", "60"), OUT]   # latency_probe parses argv at import
import latency_probe as lp                                # reuses req/run/analyse and the operator knobs

os.makedirs(OUT, exist_ok=True)
lp.OUT, lp.DUR = OUT, float(os.environ.get("DUR", "60"))
REGS = ["0x380c", "0x380d", "0x380e", "0x380f", "0x3815", "0x3035", "0x3036", "0x3037", "0x3108", "0x3824", "0x3500", "0x3501", "0x3502"]


def regs(ip):
    out = {}
    for r in REGS:
        try: out[r] = lp.req(f"/api/nodes/{ip}/command", {"cmd": f"cam reg {r}"}, "POST").strip().splitlines()[-1]
        except Exception as e: out[r] = f"ERR {e}"
    return out


res = {"rates": []}
for f in RATES:
    try:
        r = lp.run(f)
    except Exception as e:                               # a rate the firmware rejects is a result too
        r = {"fps": f, "error": repr(e)}
    res["rates"].append(r)
    json.dump(res, open(os.path.join(OUT, "fps_ceiling.stats.json"), "w"), indent=1)
    print(f, {k: r.get(k) for k in ("packets", "error")}, flush=True)

# registers after the last run, camera restarted at the highest rate so the sensor timing is the one in use
ns = {n["node_id"]: n["ip_address"] for n in json.loads(lp.req("/api/nodes")) if n.get("online")}
lp.req("/api/tracking/start", {"target_node_ids": sorted(ns), "resolution": "vga", "fps": RATES[-1], **lp.KNOBS}, "POST")
time.sleep(10)
res["registers"] = {str(k): regs(ip) for k, ip in ns.items()}
res["status"] = json.loads(lp.req("/api/tracking/status"))
lp.req("/api/tracking/stop", {}, "POST")
json.dump(res, open(os.path.join(OUT, "fps_ceiling.stats.json"), "w"), indent=1)
print("done")
