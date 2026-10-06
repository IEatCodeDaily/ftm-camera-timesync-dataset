# Timesync experimental report and retained evidence

Open `timesync_report.pdf` for the report or `timesync_report.md` for editable text. The requested academic-research-skills guidance was used at commit `8e4c8777648cdb3a1b8c01e956cf902121216aa0`; the report states its scope of use.

## What was measured

Four ESP32-S3 nodes, node 2 reference, GPIO41 on analyzer D0-D3, 16 MS/s, MCPWM pulse output. The new common-firmware comparison completed nine 60-second recordings: one live FTM and two each of local MAC/no timesync, simple NTP-style, AP TSF and frozen FTM. All nine raw signals were clean under the complete-pulse rule. Eight had cadence failures. The intended fifteen recordings were not completed because sustained acquisitions repeatedly timed out. Short and failed attempts are retained, not counted as successful repetitions.

The simple NTP-style implementation is a private software four-timestamp UDP estimator, not an RFC NTP/SNTP client. Frozen FTM is holdover after initial synchronization. No-sync affects the clock mapping used for output; background FTM traffic remains active. Statistics measure nearest one-second GPIO phase, not absolute epoch or optical exposure accuracy.

## Files

- `new-acquisition/`: raw transition CSV, native Saleae captures, commands, health, firmware identities and failed-acquisition logs.
- `historical-evidence/`: copies of original 8 September data and notes, preserving paths relative to the source repository. `historical_manifest.json` records copy hashes.
- `analysis/`: per-run/per-node statistics, all matched offsets, classifications, figures and raw-input hashes.
- `firmware/3939b066398cec36/`: selected source snapshot, compiled image/ELF, build log and host arithmetic checks. The folder name is an image-validation hash prefix; installed runtime ID is `507f8d74b8d34447`, as confirmed by the ELF hash. This is not a full standalone ESP-IDF distribution.
- `ntp_method.md`, `ntp_hardware_audit.md`: exact baseline and live update checks.
- `noise_forensics.md`, `gpio41_audit.md`: reset-associated signal recovery and conditional board-pin investigation. Exact noise cause and board revision remain unknown.
- `session_log.json`, `hardware-release.json`: session interventions and final handoff state. Hardware was restored to OFF/MCPWM-FTM/GPIO41 LOW before release to the FPS task; later hardware changes are outside this dataset.
- `evidence_review.md`, `final_review.md`: bounded independent evidence and method review; these are not journal peer review.
- `package_manifest.json`: SHA256 hashes of the delivered evidence package, excluding itself and the ZIP.

## Reproduce the analysis

Python 3 with `numpy`, `matplotlib`, `reportlab` and `pillow` is required. Run from any working directory:

```powershell
python analyze.py
python build_report.py
```

The scripts locate input paths relative to their own directory. `analyze.py` prefers the packaged historical evidence and otherwise falls back to `E:/Projects/wireless-ir-mocap`. PDF generation currently uses Windows Arial fonts in `C:/Windows/Fonts`; adapt the three font paths for another OS. `pymupdf` can render the PDF for visual review. Original environment: `E:/Projects/wireless-ir-mocap/test-output/saleae-automation-env/Scripts/python.exe`; dependency versions are recorded in `environment_versions.json`.

`acquire.py` and `check_signal.py` operate physical hardware and require the project Studio server, four configured nodes and Saleae Logic automation. They are retained for provenance; do not run them merely to regenerate the report. The archived executable is the measured image, and the live source workspace may subsequently change.

The analysis uses every transition, qualifies complete HIGH pulses of 40-65 ms, and matches nearest reference edges one-to-one within 0.5 seconds. No outlier deletion, bias correction or detrending is applied. Check counts, missing cycles and classification alongside phase statistics. CSV timestamps and the analyzer timebase are not an absolute timing calibration.
