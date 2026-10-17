#!/bin/bash
# Data campaign 2026-10-17. Exposure/gain are NEVER changed (auto-tuned 18:57 by the operator).
# Detector knobs = operator's tracking_start.json (threshold 107, min_area 1).
set -u
D=/mnt/e/Projects/ftm-camera-timesync-dataset; C=$D/campaign-2026-10-17; PY=~/.local/paperenv/.venv/bin/python
step(){ echo "=== $(date +%T) $*"; }
cd $C
step A ablation firmware ladder
( cd ~/thesis/book/measure && THRESH=107 MIN_AREA=1 $PY ablation.py runs-fw2610 60 F0_vskip2 F1_vskip4 F2_fast F2_fast30 F2_fast20 F0_vskip2_20 ) || echo "A FAILED"
step B capture-path hw timing A/B
$PY $D/tracking/tracking_ab.py 90 $C/ab || echo "B FAILED"
step C latency probe
( mkdir -p $C/latency && cd $C/latency && VSKIP=4 LOWLAT=on $PY $D/tracking/latency_probe.py 90 fast 35 30 20 ) || echo "C FAILED"
step D arm static reference
$PY track.py start
$PY $D/arm-reference/arm_reference.py $C/arm-reference.json 2 || echo "D FAILED"
step E arm dynamic sweeps
for sp in 15 50 100; do $PY arm_speed.py $sp; $PY $D/arm-reference/arm_dynamic.py $C/seg-speed$sp.json speed$sp 4 || echo "E$sp FAILED"; done
$PY arm_speed.py 50
$PY track.py copy $C/arm-session.mcap
step F wrap run 40 min
$PY $D/wrap-run/wrap_run.py 2400 $C/wrap-40min.jsonl || echo "F FAILED"
step G long static run
$PY track.py start
echo "long run started $(date +%s)" > $C/long-run.started
step DONE
