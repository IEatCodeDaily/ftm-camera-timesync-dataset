"""Dynamic run: drive the Dobot through sweeps while tracking records.

Writes a JSON log of segment boundaries (WSL wall clock ns) next to the
output name; the motion itself is recovered from the tracked trajectory.
Usage: python3 arm_dynamic.py <log.json> <label> [reps]
"""
import json, sys, time, math, urllib.request

API = "http://127.0.0.1:3001/api"
H = {"x-mocap-studio": "1", "Content-Type": "application/json"}


def get(p):
    return json.loads(urllib.request.urlopen(urllib.request.Request(API + p, headers=H), timeout=20).read())


def post(p, body):
    return urllib.request.urlopen(urllib.request.Request(API + p, data=json.dumps(body).encode(), headers=H, method="POST"), timeout=20).read()


def wait_idle(timeout=20.0):
    t0 = time.time(); last = None
    while time.time() - t0 < timeout:
        s = get("/arm")
        if s.get("error") or s.get("alarms"):
            raise RuntimeError(f"arm: {s.get('error')} {s.get('alarms')}")
        p = s["pose"]["xyz"]
        if s["mode"] == "idle" and last and max(abs(a - b) for a, b in zip(p, last)) < 0.05:
            return s
        last = p; time.sleep(0.2)
    raise TimeoutError("arm did not settle")


def pol(r, yaw, z):
    t = math.radians(yaw); return [round(r * math.cos(t), 1), round(r * math.sin(t), 1), z]


# Inside the calibrated volume (r 175..275, yaw -35..35, z -5..105).
SWEEPS = {
    "yaw":    (pol(240, -35, 50), pol(240, 35, 50)),
    "radial": (pol(175, 0, 50), pol(275, 0, 50)),
    "vert":   (pol(230, 15, -5), pol(230, 15, 105)),
}

if __name__ == "__main__":
    out, label = sys.argv[1], sys.argv[2]; reps = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    s = get("/arm"); assert s["homed"] and s["trusted"], s.get("untrusted_why")
    log = {"label": label, "speed_pct": s["config"]["speed_pct"], "segments": []}
    post("/arm/follow", {"enabled": False}); time.sleep(0.5)
    for name, (a, b) in SWEEPS.items():
        post("/arm/move", {"xyz": a}); wait_idle(); time.sleep(1.0)
        for i in range(reps):
            for p in (b, a):
                t0 = time.time_ns(); post("/arm/move", {"xyz": p}); wait_idle(); t1 = time.time_ns()
                log["segments"].append({"sweep": name, "to": p, "t0_ns": t0, "t1_ns": t1, "dur_s": (t1 - t0) / 1e9})
                print(f"{label} {name} {i} -> {p} {(t1 - t0) / 1e9:.2f}s", flush=True)
                time.sleep(0.5)
    json.dump(log, open(out, "w"), indent=1)
