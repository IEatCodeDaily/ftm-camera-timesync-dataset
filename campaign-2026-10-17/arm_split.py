"""Split arm return error from tracker drift (2026-10-19). Reads arm-backlash*.json
(same-side A / opposite-side B revisits with the static Calibrator logged) and
restart-settle.json (tracking restarts with pixel-bias learning on/off). Writes
arm-split.stats.json; every number the paper prints comes from there."""
import json, math, statistics as st
from pathlib import Path
D = Path(__file__).parent
cen = lambda c: [sum(p[k] for p in c) / 3 for k in range(3)]

def revisits(path):
    # Out-of-reach poses (r 200 at z 85) stop short at a different point each visit:
    # keep only visits where the arm's own pose reached the command.
    allrows = [r for r in json.load(open(path))["rows"] if "err" in r]
    rows = [r for r in allrows if math.dist(r["pose_xyz"], r["cmd"]) < 1.0]
    out = {}
    for ph in ("A", "B"):
        by = {}
        for r in rows:
            if r["phase"] == ph: by.setdefault(tuple(r["cmd"]), []).append(r)
        pairs = [v[:2] for v in by.values() if len(v) >= 2]
        ee = [math.dist(a["ir_world"], b["ir_world"]) for a, b in pairs]
        cal = [math.dist(cen(a["calibrator"]), cen(b["calibrator"])) for a, b in pairs if a["calibrator"] and b["calibrator"]]
        # arm part: marker move minus the calibrator's (world) move between the same two visits
        arm = [math.dist([x - y for x, y in zip(a["ir_world"], b["ir_world"])],
                         [x - y for x, y in zip(cen(a["calibrator"]), cen(b["calibrator"]))])
               for a, b in pairs if a["calibrator"] and b["calibrator"]]
        out[ph] = {"pairs": len(pairs), "ee_median_mm": st.median(ee), "cal_median_mm": st.median(cal),
                   "arm_median_mm": st.median(arm), "arm_p90_mm": sorted(arm)[int(0.9 * (len(arm) - 1))]}
    out["visits"] = len(rows); out["unreached"] = len(allrows) - len(rows)
    err = [r["err_norm"] if "err_norm" in r else math.hypot(*r["err"]) for r in rows if r["phase"] == "B"]
    out["B"]["abs_err_median_mm"] = st.median(err); out["B"]["abs_err_max_mm"] = max(err)
    return out

def settle(path):
    out = {}
    for r in json.load(open(path))["runs"]:
        R = [x for x in r["rows"] if x["cal"]]
        ref = [st.mean(cen(x["cal"])[k] for x in R if x["t"] >= 180) for k in range(3)]
        early = [math.dist(cen(x["cal"]), ref) for x in R if x["t"] <= 8]
        late_rms = math.sqrt(st.mean(math.dist(cen(x["cal"]), ref) ** 2 for x in R if x["t"] >= 180))
        settled = next((x["t"] for x in R if all(math.dist(cen(y["cal"]), ref) < 1.0 for y in R if y["t"] >= x["t"])), None)
        out[r["label"]] = {"shift_first8s_mm": st.median(early), "settle_s": settled, "late_rms_mm": late_rms}
    return out

if __name__ == "__main__":
    s = {"settle": settle(D / "restart-settle.json")}
    for name in ("arm-backlash.json", "arm-backlash-biasoff.json"):
        if (D / name).exists(): s[name] = revisits(D / name)
    json.dump(s, open(D / "arm-split.stats.json", "w"), indent=1)
    print(json.dumps(s, indent=1))
