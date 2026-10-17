import json,time,urllib.request,statistics as st,sys
B,H="http://127.0.0.1:3001/api",{"x-mocap-studio":"1","Content-Type":"application/json"}
g=lambda: json.loads(urllib.request.urlopen(urllib.request.Request(B+"/arm",headers=H)).read())
def at(p):
    urllib.request.urlopen(urllib.request.Request(B+"/arm/move",json.dumps({"xyz":p}).encode(),H)).read(); time.sleep(4)
    e=[];ir=[]
    for _ in range(20):
        s=g()
        if s.get("ee_seen") and s.get("ee_ir"): e.append([a-b for a,b in zip(s["ee_ir"],s["ee_reported_world"])]); ir.append(s["ee_ir"])
        time.sleep(0.05)
    m=[round(st.median(c),2) for c in zip(*e)] if e else None
    print(sys.argv[1],p,"n",len(e),"err",m,"ir",[round(st.median(c),1) for c in zip(*ir)] if ir else None,flush=True)
for p in ([200.0,0.0,50],[250.0,0.0,50]): at(p)
