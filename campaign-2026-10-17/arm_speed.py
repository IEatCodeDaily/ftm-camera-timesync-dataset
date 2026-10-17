"""Set the Dobot controller speed (%) through Studio. The driver applies speed_pct
only when it connects, so reconnect and re-home after the config change."""
import json, sys, time, urllib.request
B, H = "http://127.0.0.1:3001/api", {"x-mocap-studio": "1", "Content-Type": "application/json"}
req = lambda p, b=None, m=None: urllib.request.urlopen(urllib.request.Request(B + p, None if b is None else json.dumps(b).encode(), H, method=m), timeout=30).read()
s = json.loads(req("/arm")); c = s["config"]; c["speed_pct"] = float(sys.argv[1])
req("/arm/config", c, "PUT")
port = s["connected"].split()[-1]
req("/arm/connect", {"driver": "dobot", "port": port}); time.sleep(3)
req("/arm/home", {})
for _ in range(60):
    time.sleep(2); s = json.loads(req("/arm"))
    if s["homed"] and not s["homing"] and s["mode"] == "idle": break
else: sys.exit("arm did not re-home")
print("speed", s["config"]["speed_pct"], "homed at", s["pose"]["xyz"], flush=True)
