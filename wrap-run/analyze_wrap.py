"""Analyse a wrap_run.py log: per-node FREX trigger-to-capture delay and cross-node spread over time.

Usage: python3 analyze_wrap.py wrap-40min.jsonl
Asserts nothing about pass/fail; prints the numbers quoted in the paper and writes <log>.stats.json.
"""
import collections, json, statistics as st, sys

FTM_WRAP_S = 2**48 / 1e12          # 48-bit picosecond air-field stamps
MAC_WRAP_S = 2**32 / 1e6           # 32-bit microsecond MAC counter
LATE_US = 10_000                   # frame returned >10 ms after its trigger = stale/late

L = [json.loads(l) for l in open(sys.argv[1])]
by = collections.defaultdict(dict)
for x in L:
    if x["kind"] == "f":
        by[x["tick"]][x["node"]] = x["cap"]
nodes = sorted({n for v in by.values() for n in v})
t0, t1 = min(by), max(by)
dur_s = (t1 - t0) / 1e6
period = st.mode(b - a for a, b in zip(sorted(by), sorted(by)[1:]))
slots = (t1 - t0) // period + 1

lat = {n: [v[n] - t for t, v in by.items() if n in v] for n in nodes}
late = sum(l > LATE_US for ls in lat.values() for l in ls)
on_time = {t: v for t, v in by.items() if all(c - t < LATE_US for c in v.values())}
full = [v for v in on_time.values() if len(v) == len(nodes)]
spread = sorted(max(v.values()) - min(v.values()) for v in full)
med = {n: st.median([l for l in lat[n] if l < LATE_US]) for n in nodes}
jit = sorted(max(c - t - med[n] for n, c in v.items()) - min(c - t - med[n] for n, c in v.items())
             for t, v in on_time.items() if len(v) == len(nodes))

# Drift check: per-node median delay in 1-min bins; a wrap fault would show as a step.
bins = {n: collections.defaultdict(list) for n in nodes}
for t, v in on_time.items():
    for n, c in v.items():
        bins[n][(t - t0) // 60_000_000].append(c - t)
bin_med = {n: [st.median(b) for _, b in sorted(bins[n].items())] for n in nodes}
step = {n: max(m) - min(m) for n, m in bin_med.items()}

# Initiators = nodes that ever reported role NODE with a valid model; the responder never carries one
# (one health poll mislabelled the responder as NODE with sync_age None, so role alone is not enough).
health = [x for x in L if x["kind"] == "health"]
initiators = {n["node_id"] for h in health for n in h["n"] if n["role"] == "NODE" and n["sync_valid"]}
lost = sum(1 for h in health for n in h["n"] if n["node_id"] in initiators and not n["sync_valid"])
checks = sum(1 for h in health for n in h["n"] if n["node_id"] in initiators)

q = lambda s, p: s[round(p * (len(s) - 1))]
out = {
    "duration_s": dur_s, "period_us": period, "slots": slots, "nodes": nodes,
    "frames": sum(len(v) for v in by.values()), "late_frames": late,
    "full_on_time_slots": len(full), "full_slot_yield": len(full) / slots,
    # The logger polls an HTTP snapshot of the latest frames; when a poll times out every node's frames
    # for that interval are lost together. Yield among slots the logger saw at all isolates node drops.
    "slots_logged": len(by), "full_yield_of_logged": sum(len(v) == len(nodes) for v in by.values()) / len(by),
    "frames_per_node": {n: sum(n in v for v in by.values()) for n in nodes},
    "spread_us": {"median": q(spread, .5), "p95": q(spread, .95), "p99": q(spread, .99), "max": spread[-1]},
    "per_node_median_delay_us": med,
    "jitter_spread_us": {"median": q(jit, .5), "p95": q(jit, .95), "max": jit[-1]},
    "per_node_1min_median_range_us": step,
    "ftm_wraps_crossed": int(dur_s // FTM_WRAP_S), "mac_wrap_s": MAC_WRAP_S,
    "initiators": sorted(initiators), "sync_lost_checks": lost, "sync_checks": checks,
    "logger_errors": sum(x["kind"] == "err" for x in L),
}
json.dump(out, open(sys.argv[1] + ".stats.json", "w"), indent=1)
print(json.dumps(out, indent=1))
