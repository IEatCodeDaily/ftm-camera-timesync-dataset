"""Per-node fps cliff probe through the firmware's own `mode live` (bypasses Studio's 38-fps API gate).
Reads `cam stats` after a settle and reports the node's achieved slot rate.
Usage: py -3.11 fps_cliff_node.py IP[,IP...] FPS [VTS_HEX ...]   (VTS rows written after start, e.g. 0x2bc = 700)
LOWLAT=on|off (default on). All listed nodes run the same rate on their FTM-scheduled slots.
"""
import json, os, re, sys, time, urllib.request
B, H = "http://127.0.0.1:3001", {"Content-Type": "application/json", "x-mocap-studio": "1"}
ips, fps, vts = sys.argv[1].split(","), int(sys.argv[2]), [int(v, 16) for v in sys.argv[3:]]
LOWLAT = os.environ.get("LOWLAT", "on")


def cmd(c, ip):
    r = urllib.request.Request(f"{B}/api/nodes/{ip}/command", json.dumps({"cmd": c}).encode(), H, method="POST")
    try: return urllib.request.urlopen(r, timeout=60).read().decode()
    except Exception as e: return f"ERR {e}"


def frames(ip):
    s = cmd("cam stats", ip)
    m = re.search(r"track: running=1 .*? frames=(\d+) .*?capture_fail=(\d+) slots=(\d+) skipped=(\d+)", s)
    a = re.search(r"track age us: ready=(\d+)/(\d+)/([\d.]+)", s)
    return (tuple(map(int, m.groups())) if m else None), (a.groups() if a else None), s


for ip in ips:
    for c in ("cam hwstamp on", "cam hwtrig on", f"cam lowlat {LOWLAT}", "cam vskip 4"): cmd(c, ip)
for ip in ips: print(ip, "mode:", cmd(f"mode live {fps} vga --nomesh", ip).strip()[-60:])
time.sleep(8)
for v in [None] + vts:
    if v is not None:
        for ip in ips: cmd(f"cam reg 0x380e 0x{v >> 8:02x}", ip); cmd(f"cam reg 0x380f 0x{v & 0xff:02x}", ip)
        time.sleep(4)
    n0 = {ip: frames(ip)[0] for ip in ips}; t0 = time.time(); time.sleep(10)
    n1 = {ip: frames(ip) for ip in ips}; dt = time.time() - t0
    for ip in ips:
        (b, age, st), a_ = n1[ip], n0[ip]
        if a_ and b:
            d = [y - x for x, y in zip(a_, b)]                # frames, capture_fail, slots, skipped over dt
            print(json.dumps({"ip": ip, "fps_target": fps, "lowlat": LOWLAT, "vts": v or 984, "achieved_fps": round(d[0] / dt, 2),
                              "slot_rate": round(d[2] / dt, 2), "capture_fail": d[1], "skipped": d[3], "ready_last_max_mean_us": age}), flush=True)
        else:
            print(ip, "no track stats:", st[-300:])
for ip in ips:
    cmd("cam reg 0x380e 0x03", ip); cmd("cam reg 0x380f 0xd8", ip); cmd("cam lowlat on", ip)
    print(ip, "stop:", cmd("mode off", ip).strip()[-40:])
