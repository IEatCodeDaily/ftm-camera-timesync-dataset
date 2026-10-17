#!/bin/bash
# Resume from D. Exposure/gain NEVER changed. Arm at 50 % drifted ~0.8 mm per move cycle
# (open-loop lost steps), so static runs at 15 % and every dynamic speed is followed
# by a re-home and a static drift check at 2 poses.
set -u
D=/mnt/e/Projects/ftm-camera-timesync-dataset; C=$D/campaign-2026-10-17; PY=~/.local/paperenv/.venv/bin/python
step(){ echo "=== $(date +%T) $*"; }
cd $C
step D arm static reference, speed 15
$PY track.py start
$PY arm_speed.py 15
$PY $D/arm-reference/arm_reference.py $C/arm-reference.json 2 || echo "D FAILED"
step E arm dynamic sweeps
for sp in 15 50 100; do
  $PY arm_speed.py $sp
  $PY $D/arm-reference/arm_dynamic.py $C/seg-speed$sp.json speed$sp 4 || echo "E$sp FAILED"
  $PY errcheck.py after-speed$sp
  $PY arm_speed.py 15
  $PY errcheck.py rehomed-after-speed$sp
done
$PY track.py copy $C/arm-session.mcap
step F wrap run 40 min
$PY $D/wrap-run/wrap_run.py 2400 $C/wrap-40min.jsonl || echo "F FAILED"
step G long static run
$PY track.py start
date +%s > $C/long-run.started
step DONE
