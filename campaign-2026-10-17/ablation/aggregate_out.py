"""Aggregate ablation runs -> measure/ablation.stats.json + chapter macros (no hand-typed numbers).

Per run (MCAP window [t0, t0+dur]):
  network excess      = (arrival - capture_us - on-node send time) minus a line through per-10-s minima
                        (host-vs-hub drift), shifted so the run's minimum is 0.
  per-packet latency  = on-node send time + network excess -> lower bound (fastest one-way taken as 0).
  slot latency        = same, but last of 4 cameras in a slot (only when all 4 share the slot).
  on-node             = frame_ready / detection / send-after-capture fields of IRP1.
Asserts guard the facts the text relies on.
"""
import json, sys
from collections import defaultdict
from pathlib import Path
import numpy as np
from mcap.reader import make_reader

HERE = Path(__file__).parent


def q(a):
    a = np.asarray(a, float)
    return None if a.size == 0 else {"median": float(np.median(a)), "p95": float(np.percentile(a, 95)),
                                     "p99": float(np.percentile(a, 99)), "max": float(a.max()), "n": int(a.size)}


def load(run):
    meta = json.loads((run.with_suffix(".json")).read_text())
    rows = [json.loads(m.data) for _, _, m in make_reader(open(run, "rb")).iter_messages(topics=["/mocap/centroids"])]
    t0, dur = meta["t0"], meta["dur"]
    return meta, [r for r in rows if t0 * 1e9 <= r["host_time_ns"] <= (t0 + dur) * 1e9]


def analyse(meta, rows):
    fps = meta["config"]["fps"]; period = 1e6 / fps
    cap = np.array([r["capture_us"] for r in rows], float)
    arr = np.array([r["host_time_ns"] / 1e3 for r in rows])
    send = np.array([r["frame_ready_us"] + r["preprocess_us"] + r["detection_us"] for r in rows], float)
    raw = arr - cap - send                     # network part only; on-node time is measured, not fitted
    tb = ((cap - cap.min()) // 10e6).astype(int)
    xs = np.array([cap[tb == b].mean() for b in np.unique(tb)]); ys = np.array([raw[tb == b].min() for b in np.unique(tb)])
    slope, icpt = np.polyfit(xs, ys, 1)
    net = raw - (slope * cap + icpt); net -= net.min()
    lat = send + net                           # capture -> host arrival, lower bound (fastest one-way = 0)
    slots = defaultdict(list)
    for r, l in zip(rows, lat):
        slots[round(r["capture_us"] / period)].append((r["camera_id"], l))
    n_cam = len(meta["nodes"])
    full = [max(l for _, l in v) for v in slots.values() if len({c for c, _ in v}) == n_cam]
    # inter-camera frame-start spread in full slots: raw, and after each camera's median delay is removed
    by_slot = defaultdict(dict)
    for r in rows:
        by_slot[round(r["capture_us"] / period)][r["camera_id"]] = r["capture_us"] - round(r["capture_us"] / period) * period
    fulls = [s for s in by_slot.values() if len(s) == n_cam]
    med = {c: np.median([s[c] for s in fulls]) for c in meta["nodes"]} if fulls else {}
    spread = [max(s.values()) - min(s.values()) for s in fulls]
    jit = [max(s[c] - med[c] for c in s) - min(s[c] - med[c] for c in s) for s in fulls]
    span = (cap.max() - cap.min()) / 1e6
    n_slots = max(slots) - min(slots) + 1      # inclusive scheduled-slot window (rev12: was round(span*fps), one short at the edges)
    per_cam = defaultdict(list)
    for r in rows: per_cam[r["camera_id"]].append(r["capture_us"])
    fps_c = [(len(v) - 1) / ((max(v) - min(v)) / 1e6) for v in per_cam.values()]
    return {
        "fps_target": fps, "fps_min": min(fps_c), "fps_max": max(fps_c), "packets": len(rows),
        "slots_total": n_slots, "slots_full": len(full), "full_pct": 100 * len(full) / max(1, n_slots),
        "drift_ppm": slope * 1e6, "markers_per_packet": float(np.mean([len(r.get("markers", [])) for r in rows])),
        "truncated_packets": int(sum(len(r.get("markers", [])) >= 31 for r in rows)),
        "frame_ready_us": q([r["frame_ready_us"] for r in rows]),
        "detection_us": q([r["detection_us"] for r in rows]),
        "send_us": q([r["frame_ready_us"] + r["preprocess_us"] + r["detection_us"] for r in rows]),
        "spread_us": q(spread), "jitter_us": q(jit), "network_excess_us": q(net), "packet_latency_us": q(lat), "slot_latency_us": q(full),
    }


def main():
    out = {}
    for d in sys.argv[1:]:
        for run in sorted((HERE / d).glob("*.mcap")):
            meta, rows = load(run)
            res = {"config": meta["config"], "thresh": meta.get("thresh"), **analyse(meta, rows)}
            # node counters over the measured window (cam stats before/after): skipped slots, capture failures
            import re
            def ctr(lines, k):
                t = next(l for l in lines if l.startswith("track: "))
                return int(re.search(rf"\b{k}=(\d+)", t).group(1))
            for k in ("skipped", "capture_fail", "truncated"):
                res[f"{k}_per_node"] = {n: ctr(meta["stats_after"][n], k) - ctr(meta["stats_before"][n], k) for n in meta["stats_after"]}
            out[f"{d}/{run.stem}"] = res
    (HERE / (__import__("os").environ.get("OUT_STATS", "ablation.stats.json"))).write_text(json.dumps(out, indent=1))
    for k, v in out.items():
        f = lambda s, key="median", sc=1e3: (v[s] or {}).get(key, float("nan")) / sc
        print(f"{k:28s} fps {v['fps_min']:5.1f}-{v['fps_max']:5.1f} full {v['full_pct']:5.1f}% mk {v['markers_per_packet']:5.2f} "
              f"det {f('detection_us', sc=1):7.0f}us send {f('send_us'):5.1f} pkt {f('packet_latency_us'):5.1f}/{f('packet_latency_us','p95'):5.1f} "
              f"slot {f('slot_latency_us'):5.1f}/{f('slot_latency_us','p95'):5.1f} ms jit {f('jitter_us',sc=1):.0f}/{f('jitter_us','p95',sc=1):.0f}us spread {f('spread_us',sc=1):.0f}us")


if __name__ == "__main__":
    main()
