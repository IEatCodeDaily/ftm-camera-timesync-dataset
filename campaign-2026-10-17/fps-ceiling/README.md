# FPS ceiling, 2026-10-18 (fw 842af0eb, VGA vskip 4, hwtrig + hwstamp + lowlat)

Question: after the pipeline optimizations, can tracking run at 40-50 fps?

## Full rig through Studio (fps_ceiling.py, 30 s per rate, operator knobs)
| target | per-camera fps | complete slots | frame ready (median) | slot last packet med / p95 |
|---|---|---|---|---|
| 35 | 35.03 x4 | 100.0 % | 18.2 ms | 27.5 / 44.9 ms |
| 38 | 37.93-38.00 | 99.6 % | 18.2 ms | 27.9 / 61.4 ms |
| 40+ | rejected by Studio: MAX_VGA_TRACKING_FPS = 38 (studio/server/src/main.rs) | | | |

## Per node through firmware `mode live` (fps_cliff_node.py, 10 s windows, bypasses the Studio gate)
Node .31 alone: 40 -> 39.7 fps (1 skip), 42 -> 41.1 (9 skips), 45 -> 40-41 (20+ skips/10 s), 50 -> 24.5 (halves).
All four nodes, lowlat on: 40 -> 35.1 / 36.5 / 39.3 / 40.7 fps; 45 -> 33-41 fps with 18-56 skips and capture_fail;
50 -> ~25 fps on every node (every other slot).
lowlat off: counters inconsistent (negative deltas = task restarted mid-window); not usable.

## VTS (0x380e/f, frame length in rows) shortened live on node .31
45 fps: VTS 984 / 800 / 720 / 640 -> 40.2 / 41.4 / 42.1 / 41.9 fps, ~20 skips per 10 s at every VTS.
50 fps: VTS 984 / 720 / 640 / 608 -> 24-25 fps at every VTS.
Shortening the frame did not move the cliff, so the sensor frame (25.3 ms at VTS 984) is not the binding limit.

## Reading
- Sensor period 25.34 ms -> 39.5 fps in theory. Measured: 38 fps clean on the rig, 40 fps clean on one node only.
- The binding term is the frame-ready time, ~18.2 ms after the slot, almost independent of VTS and rate
  (exposure + 12.4 ms readout + driver). With a 22.2 ms slot (45 fps) the loop has ~4 ms for detect + send and
  the next FREX arm, and starts missing slots; at 20 ms (50 fps) every other slot is missed.
- 40-50 fps at VGA needs a shorter frame-ready time: less readout (crop rows/ROI, QVGA) or a faster interface.

## 40 fps through Studio after lifting MAX_VGA_TRACKING_FPS 38 -> 40 (60 s per run, studio40/)
| target | per-camera fps (n0..n3) | complete 4-cam slots | frame ready | last packet med / p95 |
|---|---|---|---|---|
| 38 | 38.00 / 37.97 / 38.00 / 38.02 | 99.78 % | 18.2 ms | 27.9 / 52.1 ms |
| 40 | 39.43 / 39.78 / 40.02 / 39.95 | 97.83 % | 18.3 ms | 27.0 / 46.5 ms |
| 40 (repeat) | 39.38 / 39.67 / 40.00 / 39.85 | 97.67 % | 18.2 ms | 28.5 / 100.9 ms |
Detect at 40: pose_hz 40.0, quorum_yield 99.8 %, 0 late observations.
Studio was only the 38 cap; it keeps up at 40. Remaining loss is per-node frame skips (node 0 worst, ~1.5 %).
