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
| G long static tracking run (1st) | Studio recording | 20:50-22:16, 85 min; arm moved 21:47-21:48 (wide sweep) -> not static |
| A2 full thesis ablation on 842af0eb, 60 s/trial | thesis `measure/ablation.py` D/L/W trials, THRESH 107 / MIN_AREA 1 | `ablation-full/`, `runs-fw2610-full.log`, `ablation-fw2610.stats.json` |
| A3 bright-scene stress, threshold 40 | same, THRESH 40 | `ablation-t40/`, `runs-fw2610-t40.log` |
| E2 wide 100 % yaw sweep, +-75 deg, 6 reps | `arm_dynamic_wide.py` (WIDE=1), `dynamic_n.py` | `seg-speed100-wide.json`, `arm-wide.mcap`, `dynamic-wide.json`, `paired-wide.json` |
| F2 nodes-only wrap run, 40 min, continuous receive | `wrap_run_cont.py` (snapshot `?after=`, 2 s timeout) | `wrap2-40min.jsonl`, `wrap2.analysis.txt` |
| G2 long static tracking run (arm parked) | Studio recording | `long-run2.started` |
| H host estimator benchmark, current Studio build | `cargo test --release tracking_benchmark` | `host_bench.log` |
| R HREF row timing, vskip 4, all nodes | `cam href` | `rowtiming/` |

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
The 2026-10-16 run measured 21 ms median with 0.42 blobs/packet; today 5.1 blobs/packet. On-node send moved only
18.2 -> 18.6 ms while host arrival spread grew 2.3 -> 9.2 ms, so the difference is delivery/arrival, NOT
detector cost. The cause is not established (no controlled sparse/busy A/B); report today's 29 ms as measured.

**D. Arm static reference** (36/36 poses OK): tool point vs arm-reported point mapped through the
arm calibration: median 3.6 mm, RMS 4.8 mm. Calibration-independent length check with the thesis convention
(`accuracy_arm_n.py`: visits grouped by reached pose, 17 poses, 136 pairs, 35-185 mm):
RMS 1.76 mm, median |err| 1.24 mm, p95 3.30 mm, scale 0.990 (`acc_arm_pairs.json`; old run 0.74 mm RMS).
Static IR SD 0.14 mm median. Pass medians 6.4 vs 2.8 mm show the residual is non-stationary; whether the
arm or the registration moved is NOT separated (no independent fixed reference was recorded).

**E. Dynamic** (4-LED marker B, 12 464 labelled slots, up to 448 mm/s): inter-LED distances vs the
same recording's static distances: RMS 0.65-0.86 mm in every speed bin, every row slope (0/0.5/1).
Direction-paired |fwd-bwd|/2: 0.05-0.18 mm median. Row-slope choice does not matter at these speeds.

**F. Wrap run** (40 min, 8 FTM wraps crossed, 0 sync losses): fixed-removed jitter spread median
35 us / p95 80 us; per-node 1-min median drift range 7-20 us. Full-slot yield 56 % of expected is
limited by the LOGGER: 50 snapshot timeouts of 15 s plus 29 silent 11 s gaps; node tx counter deltas
(end - stats0) are 23 996-24 015 per node over ~2400 s (~10 Hz). Of the logged ticks 96.4 % were full,
94.9 % full and on time; 256 frames were late (> 10 ms; node 2: 185). This path is scheduled from AP TSF
(sync_capture.c), so it is a capture-path continuity test, NOT an FTM-trigger wrap test. Superseded by F2.

**A2. Full ablation on the current firmware** (`ablation-fw2610.stats.json`, aggregate.py; slot = capture to last
camera at host, median/p95):

| Trial | fps | full | detect med | send med | slot med/p95 |
|---|---|---|---|---|---|
| L0_naive10 (scalar, 1 buf, no rowstream, sw timing) | 10 | 100 % | 46.6 ms | 72.5 ms | 88.3/145.1 ms |
| L0_naive | 35 | 0 % (11.2 fps) | 46.6 ms | 74.8 ms | - |
| L1_simd | 35 | 0 % (16.7 fps) | 6.6 ms | 34.7 ms | - |
| L2_syncbufs | 35 | 100 % | 8.8 ms | 37.0 ms | 44.3/77.6 ms |
| L3_rowstream | 35 | 100 % | 0.13 ms | 28.6 ms | 39.0/123.0 ms |
| L4_hwtiming (old shipped) | 35 | 99.5 % | 0.13 ms | 28.7 ms | 36.1/76.0 ms |
| F2_fast (vskip 4 + same-slot send, new shipped) | 35 | 100 % | 0.11 ms | 18.6 ms | 29.8/73.4 ms |

Detectors at 35 fps, full-frame: scalar 47.7 ms (17.5 fps, 0 % full), SWAR 22.8 ms (95.6 %), PIE prefix 9.5 ms,
PIE full 8.8 ms (100 %). W10/W30 default vs lowlat (vskip 2): send 26.8 ms both; at vskip 2 the same-slot grab
does not shorten send (readout 24.7 ms fills the slot). Threshold 40 (10-13 blobs/packet, no truncation):
PIE 9.1 ms detect, 100 % full; scalar 16.7 % full; rowstream drops node 1 to 20 fps (57 % full).
L1_simd20 (9.5 fps, 0 full) failed to hold its rate and is not usable.

**E2. High speed** (wide +-75 deg yaw at 100 %, 6 reps): 1 274 labelled slots, tracked speed median 486 mm/s
in-segment, p95 692, max 713 mm/s; 209 in-segment slots above 500 mm/s. Inter-LED distance |error| vs static:
300-500 mm/s median 0.35 / p95 1.27 mm, 500-800 mm/s median 0.44 / p95 1.61 mm; RMS 0.71-0.79 mm (300+ bin).
Direction-paired |fwd-bwd|/2 median 0.11 mm. Speed control verified: yaw leg durations 3.66/2.03/1.63 s and
tracked yaw maxima 155/321/448 mm/s at 15/50/100 %. After every wide sweep the arm-vs-IR offset jumped to
~10 mm and returned to ~2 mm within ~45 s (re-home, `campaign2.log`/drift checks): use the dynamic data for
rigidity only, never for absolute position.

**H. Host benchmark** (current Studio source): point-path handle 7.0/31.2/83.9 us per packet for 1/4/8 markers,
clear RMS 0.66/0.68/0.80 mm (synthetic); unchanged from the historical log.

**R. Row timing** (`cam href` during tracking, all 4 nodes, vskip 4): 480 HREF rows per frame at 25.75 us
= 12.36 ms. The firmware's reported row_period_ns 25750 is correct for vskip 4 (the shipped default); for
vskip 2 data (all recordings before 2026-10-16) the true output row is 51.50 us (href-2026-10-15.txt), so
row-derived numbers of those recordings must be doubled.

## Round 2 additions (23:00-00:40)

**F2. Wrap run, continuous receive** (`wrap2-40min.jsonl`, `wrap2.analysis.txt`; HW QVGA 10 fps, AP-TSF-scheduled
capture path, logger run from Windows Python): 2400 s, 23 999 of 24 000 slots logged, 0 logger errors,
full on-time yield 98.9 % (99.5 % of logged slots full), 179 late frames, 0 sync-lost checks out of 1 437,
inter-node spread median 771 us / p99 839 us / max 1106 us, jitter median 35 us / p95 79 us.
The 56 % of F was the logger. Root cause: requests from WSL to the Windows-hosted Studio stall 5 s on about
1 % of calls (300-request A/B: WSL 4 stalls, Windows 0, max 0.03 s). Run host loggers from Windows Python.
`failed-wsl-wrap/` keeps the aborted WSL attempt.

**D2. Arm static reference, repeat** (`arm-reference-2.json`, same calibration, speed 15, re-homed):
absolute median 5.0 mm (first run 3.6 mm), repeat IR distance between visits to the same pose 4.8 mm
(first run 3.4 mm), static IR SD 0.15 mm. Pair-length check (`acc_arm_pairs-2.json`, 17 poses, 136 pairs):
RMS 1.85 mm, median 1.24->1.29 mm, scale 0.99. The arm does not return to the same point within ~4-5 mm,
so the absolute error floor is set by the arm's repeatability. Pair-length (calibration-free) is stable
across both runs. A fresh registration would not lower the floor and was not done (it rewrites the
operator's arm calibration).

**S. Time-source emulation on the new 4-LED tracks** (`sync_emulation_n.py`, `sync-emulation-wide.json`,
wide sweep, n=662 slots up to ~710 mm/s): centroid shift median / p95 for one camera offset by each source's
worst measured offset: Live FTM 0.000/0.001 mm, AP TSF 0.024/0.041 mm, FTM + row residual (612 us)
0.080/0.139 mm, NTP-style UDP 7.4/12.7 mm, no sync 24.7/47.0 mm. Emulation on real tracks, not a live run.

**G2. Long static run** started 00:40 with the arm parked at home (`tracking-<ts>.mcap`, see `long-run2.started`).
Needs >= 7.4 h to replace the old claim.

### Still open (needs a human or time)
1. Calibrator wand run against caliper geometry, 10 min moved + 5 min static (someone has to move the wand).
2. Dense-scene D0-D3 ablation (needs extra markers placed in view).
3. G2 has to keep running overnight.
4. Repoint paper `make_figures.py` and thesis `build.py` to these files (writing work, done after the data is final).

## Caveats
- D/E use the arm (open-loop steppers): at 50 % speed the arm drifted ~0.8 mm per move cycle
  (`failed-speed50/`, not used). Static runs therefore at 15 %, with a re-home + 2-pose drift check
  after each dynamic speed (`campaign2.log`: drift checks within ~1 mm).
- F logger gaps: the 56 % figure must not be reported as node yield; use node tx counts + logged-slot yield.
- G is running unattended; analyse with `../accuracy/accuracy_wand.py` style tooling once stopped.
