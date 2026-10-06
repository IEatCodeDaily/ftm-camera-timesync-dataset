# Wi-Fi FTM clock alignment for ESP32 camera nodes — measurement dataset

Raw data, analysis scripts, and figure scripts behind the letter

> R. P. Wardana and E. M. Budi, "Wi-Fi FTM Clock Alignment for Low-Cost Wireless Infrared Tracking: From Node Clocks to Rolling-Shutter Capture," submitted to *IEEE Sensors Letters*.

Four ESP32-S3 nodes (one FTM responder, three initiators) each drive a 50 ms pulse on GPIO41 once per predicted shared second. A Saleae logic analyzer records all four outputs at 16 MS/s. Five clock sources are compared on one firmware image and the same MCPWM output path: no sync, NTP-style UDP exchange, AP TSF, FTM holdover, and live FTM.

## Layout

| Path | Contents |
|---|---|
| `timesync-2026-09-09/new-acquisition/` | Common-firmware campaign (9 Sep 2026). Raw transitions (`digital.csv`), native Saleae captures (`.sal`), commands, node health, firmware identities, and failed-acquisition logs |
| `timesync-2026-09-09/historical-evidence/` | Earlier 8 Sep 2026 records (live-FTM H1–H3, the 25-s backend comparison, the camera-load record), with original repo-relative paths kept |
| `timesync-2026-09-09/analysis/` | `offsets.csv` (every matched follower-minus-reference offset), `per_run_node.csv`, classifications, and raw-input hashes |
| `timesync-2026-09-09/firmware/3939b066398cec36/` | Compiled image and ELF of the measured campaign (C1/C2) build (ELF SHA-256 `507f8d74b8d34447…`), plus a partial source excerpt (`simple_ntp.c`, `strobe_gpio.c` and headers only; not a buildable tree) |
| `timesync-2026-09-09/*.md`, `*.pdf` | Full experimental report, method notes, evidence and noise reviews |
| `sync-matrix/sync-matrix.csv` | Summary (not per-slot observations) of triggered frame-capture spread and yield for 3 cameras (QVGA/VGA, 10–60 frames/s) |
| `paper/make_figures.py` | Regenerates every figure and table in the letter from `offsets.csv` and `sync-matrix.csv`, and asserts the headline statistics. Hardware, configuration and fault-history numbers quoted in the letter are not derived here |
| `wrap-run/` | 40-min all-node run (6 Oct 2026) with no logic analyzer: every node captures FREX-triggered QVGA frames at 10 frames/s; per-frame trigger tick and capture timestamp in the shared clock, node health every 5 s. `wrap_run.py` (logger), `analyze_wrap.py` → `wrap-40min.jsonl.stats.json` |
| `wrap-run/ab-*.jsonl` | Capture-path A/B on firmware 68cee2605ffadeb1 (6 Oct 2026, same 4 nodes, QVGA 10 frames/s FREX): `ab-base-clean`/`ab-base2` = software trigger + software VSYNC stamp (10 and 5 min), `ab-hw3` = `cam hwstamp on; cam hwtrig on` (5 min, MCPWM-latched VSYNC + GPTimer-started trigger write). Same analysis script |
| `wrap-run/wrap-40min-hw.jsonl` | Same 40-min run as `wrap-40min.jsonl` on firmware 68cee2605ffadeb1 with `cam hwstamp on; cam hwtrig on` |
| `tracking/ab/` | Tracking-mode A/B (6 Oct 2026, 4 cameras, VGA 35 frames/s, 600 s each, firmware 68cee2605ffadeb1): software path vs hardware VSYNC stamp + timer-started trigger. MCAPs, node counters, `tracking_ab.py` → `tracking_ab.stats.json` |
| `tracking/` | IR tracking recordings (6 Oct 2026, MCAP of on-node centroids, VGA 35 frames/s, 3–4 cameras), the calibration files used, host-replay logs, and `analyze_tracking.py` → `TRACKING_RESULTS.md` / `stats.json` |

## Reproduce

```bash
python -m pip install matplotlib
python paper/make_figures.py        # writes paper/figures/, fails if any quoted number drifts
python timesync-2026-09-09/analyze.py   # re-derives analysis/ from the raw transitions (needs numpy)
```

Rules used by the analysis: a pulse counts when its HIGH width is 40–65 ms; follower edges are matched one-to-one to reference edges within ±0.5 s; no outlier removal, glitch filtering or detrending. Quantiles use the nearest-sample rule, index round(p(n−1)).

## Scope and limits

- Offsets are electrical GPIO phase relative to the reference node. They are not optical exposure times.
- The 40-min `wrap-run/` crosses 8 FTM 48-bit timestamp wraps with no step (camera-timestamp resolution, tens of µs).
- Nine of fifteen planned campaign records finished before analyzer timeouts stopped the campaign. Short and failed attempts are kept but are not counted.
- `acquire.py` and `check_signal.py` drive physical hardware and are kept for provenance only.
- Some JSON logs contain private-subnet node addresses (192.168.137.x) from an isolated laptop hotspot.

- Raw logic-analyzer captures (`digital.csv`) exist for every GPIO record in `offsets.csv`. Firmware images are archived only for the campaign build; the other measured builds are identified by ELF SHA-256 prefix only: `33a74b5c18b078e9` (H1, H2), `e0ac899e423e2d93` (H3), `34e1a3df22c26af8` (backend comparison). Complete buildable firmware source is not part of this dataset.
- The free-running throughput record quoted in the letter is not included.

## License

Data: CC BY 4.0. Code: MIT. See `LICENSE`.
