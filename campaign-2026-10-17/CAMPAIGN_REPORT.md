# Data campaign 2026-10-17 (firmware 842af0ebb2e788b9, 4 nodes, VGA)

Every number below is printed by `summarize.py` from the raw files in this folder (`campaign.stats.json`).
Exposure/gain were never changed by any script: per-camera values from the operator's auto-tune
(18:57), diffed against `tune-before.txt` after the campaign. Detector knobs = operator's
`rig/tracking_start.json` (threshold 107, min_area 1). Rig = `rig/` (intrinsics, wand extrinsics
of 18:05, arm calibration, rigid bodies).

| Step | Script | Raw data |
|---|---|---|
| A firmware ladder, 60 s/trial | thesis `measure/ablation.py` F* trials | `~/thesis/book/measure/runs-fw2610/` |
| B capture timing hw vs sw, 90 s each | `../tracking/tracking_ab.py` | `ab/` |
| C latency, shipped config, 90 s/rate | `../tracking/latency_probe.py` | `latency/fast/` |
| D arm static reference, 18 poses x 2, speed 15 % | `../arm-reference/arm_reference.py` | `arm-reference.json` |
| E arm sweeps 15/50/100 %, 4 reps | `../arm-reference/arm_dynamic.py`, `dynamic_n.py`, `../arm-reference/dynamic_paired.py` | `seg-speed*.json`, `arm-session.mcap`, `dynamic.json`, `paired.json` |
| F nodes-only FREX wrap run, 40 min, 10 fps QVGA | `../wrap-run/wrap_run.py` | `wrap-40min.jsonl`, `wrap.analysis.txt` |
| G long static tracking run | Studio recording | started 20:50, still recording |

## Results

**A. Firmware ladder** (median e2e = capture to last camera at host; full = 4-camera slots / expected)

| Trial | fps | vskip | same-slot send | full | send med | e2e med | e2e p95 |
|---|---|---|---|---|---|---|---|
| F0_vskip2 | 35 | 2 | off | 99.7 % | 28.7 ms | 37.8 ms | 101.8 ms |
| F1_vskip4 | 35 | 4 | off | 44.0 % | 20.2 ms | 44.9 ms | 246 ms |
| F2_fast (shipped) | 35 | 4 | on | 100 % | 18.6 ms | 29.8 ms | 73.4 ms |
| F2_fast30 | 30 | 4 | on | 100 % | 18.4 ms | 29.2 ms | 45.5 ms |
| F2_fast20 | 20 | 4 | on | 100 % | 18.4 ms | 30.9 ms | 54.9 ms |
| F0_vskip2_20 | 20 | 2 | off | 99.9 % | 26.7 ms | 37.5 ms | 58.8 ms |

4x vertical sampling alone (F1) breaks the pipelined grab at 35 fps (44 % full slots); it only works
together with the same-slot send (F2). Send-after-capture is flat at 18.4-18.6 ms for 20-35 fps.

**B. Capture timing** (35 fps, 90 s): hardware trigger+stamp jitter (fixed per-camera offset removed)
median 1.4 us / p99 3.7 us vs software 13.7 us / 318 us; full-slot yield 100 % vs 99.6 %.
The raw cross-camera spread (~540 us) is a fixed per-camera offset (node 1 +0.4-0.5 ms), not jitter.

**C. Latency** (shipped config, 90 s per rate): send-after-capture median 18.6/18.5/18.7 ms at
35/30/20 fps; capture -> last camera at host median 29.1/29.4/32.1 ms, p95 60.6/54.5/65.3 ms.
Wi-Fi delivery excess median 2.7-3.8 ms, p95 25-32 ms: the Wi-Fi tail dominates p95.
(The 2026-10-16 run measured 21 ms median at an empty scene; today's scene has 5-7 blobs per camera.)

**D. Arm static reference** (36/36 poses OK): tool point vs arm-reported point mapped through the
arm calibration: median 3.6 mm, RMS 4.8 mm. Calibration-independent length check (distance between
two arm poses vs tracked distance, 609 pairs): RMS 2.24 mm, median |err| 1.57 mm. Static IR SD
0.14 mm median. Error fell from 6.4 mm median on pass 1 to 2.8 mm on pass 2, i.e. part of the
absolute error is arm drift (open-loop steppers), not the tracker.

**E. Dynamic** (4-LED marker B, 12 464 labelled slots, up to 448 mm/s): inter-LED distances vs the
same recording's static distances: RMS 0.65-0.86 mm in every speed bin, every row slope (0/0.5/1).
Direction-paired |fwd-bwd|/2: 0.05-0.18 mm median. Row-slope choice does not matter at these speeds.

**F. Wrap run** (40 min, 8 FTM wraps crossed, 0 sync losses): fixed-removed jitter spread median
35 us / p95 80 us; per-node 1-min median drift range 7-20 us. Full-slot yield 56 % of expected is a
LOGGER artefact: the nodes sent 24 239-24 278 frames each (101 % of 24 000), but the host snapshot
poller timed out 50 times (15 s each, `err` rows). Of the slots the poller did log, 96.4 % were full.

## Caveats
- D/E use the arm (open-loop steppers): at 50 % speed the arm drifted ~0.8 mm per move cycle
  (`failed-speed50/`, not used). Static runs therefore at 15 %, with a re-home + 2-pose drift check
  after each dynamic speed (`campaign2.log`: drift checks within ~1 mm).
- F logger gaps: the 56 % figure must not be reported as node yield; use node tx counts + logged-slot yield.
- G is running unattended; analyse with `../accuracy/accuracy_wand.py` style tooling once stopped.
