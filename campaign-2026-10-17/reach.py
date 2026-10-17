import json,sys,time,math,urllib.request
B,H="http://127.0.0.1:3001/api",{"x-mocap-studio":"1","Content-Type":"application/json"}
g=lambda: json.loads(urllib.request.urlopen(urllib.request.Request(B+"/arm",headers=H)).read())
def mv(p):
    urllib.request.urlopen(urllib.request.Request(B+"/arm/move",json.dumps({"xyz":p}).encode(),H)).read(); time.sleep(3.5)
    s=g(); seen=sum(1 for _ in range(10) if (g().get("ee_seen"), time.sleep(.05))[0]); return s["error"], s["pose"]["xyz"] if s["pose"] else None, seen
for r in (260, 275):
    for yaw in (-75,-60,-45,45,60,75):
        t=math.radians(yaw); p=[round(r*math.cos(t),1),round(r*math.sin(t),1),50]
        print(r,yaw,mv(p),flush=True)
mv([250.0,0.0,50])
