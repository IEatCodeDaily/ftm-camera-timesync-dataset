#!/bin/bash
# Long static run watchdog: every 5 min log tracking health; restart tracking (operator knobs) if it stopped.
PY=~/.local/paperenv/.venv/bin/python; B=http://127.0.0.1:3001
while true; do
  s=$(curl -s -m 10 $B/api/tracking/status | python3 -c 'import json,sys;d=json.load(sys.stdin);t=d.get("detect") or {};r=d["recording"];print(d["running"],d["phase"],round(t.get("pose_hz") or 0,1),round(t.get("quorum_yield") or 0,4),r["written"],r["dropped"],r["path"].split("\\")[-1])' 2>&1)
  echo "$(date +%T) $s"
  case "$s" in True*) ;; *) echo "$(date +%T) RESTART"; $PY track.py start;; esac
  sleep 300
done
