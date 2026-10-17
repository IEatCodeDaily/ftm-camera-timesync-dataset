"""start|stop|copy <dst>: tracking with the operator's saved knobs (tracking_start.json) - never touches exposure/gain."""
import json, shutil, sys, time, urllib.request
B, H = "http://127.0.0.1:3001", {"x-mocap-studio": "1", "Content-Type": "application/json"}
req = lambda p, b=None: urllib.request.urlopen(urllib.request.Request(B + p, None if b is None else json.dumps(b).encode(), H), timeout=180).read().decode()
if sys.argv[1] == "start":
    spec = json.load(open("/mnt/e/Projects/wireless-ir-mocap/mocap/studio/tracking_start.json"))
    for a in range(3):
        try: print(req("/api/tracking/start", spec)); break
        except Exception as e: print("start retry", e); req("/api/tracking/stop", {}); time.sleep(10)
    time.sleep(8)
elif sys.argv[1] == "stop":
    s = json.loads(req("/api/tracking/status")); print(req("/api/tracking/stop", {})); time.sleep(5)
    print(s["recording"]["path"])
elif sys.argv[1] == "copy":
    s = json.loads(req("/api/tracking/status")); src = s["recording"]["path"].replace("E:\\", "/mnt/e/").replace("\\", "/")
    print(req("/api/tracking/stop", {})); time.sleep(5); shutil.copy(src, sys.argv[2]); print("copied", src, "->", sys.argv[2])
