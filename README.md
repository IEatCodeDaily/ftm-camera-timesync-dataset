# Wi-Fi FTM clock alignment for ESP32 camera nodes — measurement dataset

Raw data, analysis scripts, and figure scripts behind the letter

> R. P. Wardana and E. M. Budi, "Microsecond Clock Alignment of Wireless ESP32 Tracking Cameras Using Wi-Fi Fine Timing Measurement," submitted to *IEEE Sensors Letters*.

Four ESP32-S3 nodes (one FTM responder, three initiators) each drive a 50 ms pulse on GPIO41 once per predicted shared second. A Saleae logic analyzer records all four outputs at 16 MS/s. Five clock sources are compared on one firmware image and the same MCPWM output path: no sync, NTP-style UDP exchange, AP TSF, FTM holdover, and live FTM.

## Layout

| Path | Contents |
|---|---|
| `timesync-2026-09-09/new-acquisition/` | Common-firmware campaign (9 Sep 2026). Raw transitions (`digital.csv`), native Saleae captures (`.sal`), commands, node health, firmware identities, and failed-acquisition logs |
| `timesync-2026-09-09/historical-evidence/` | Earlier 8 Sep 2026 records (live-FTM H1–H3, the 25-s backend comparison, the camera-load record), with original repo-relative paths kept |
| `timesync-2026-09-09/analysis/` | `offsets.csv` (every matched follower-minus-reference offset), `per_run_node.csv`, classifications, and raw-input hashes |
| `timesync-2026-09-09/firmware/3939b066398cec36/` | Source snapshot, compiled image and ELF of the measured campaign build (runtime ID `507f8d74b8d34447`) |
| `timesync-2026-09-09/*.md`, `*.pdf` | Full experimental report, method notes, evidence and noise reviews |
| `sync-matrix/sync-matrix.csv` | Triggered frame-capture spread and yield for 3 cameras (QVGA/VGA, 10–60 frames/s) |
| `paper/make_figures.py` | Regenerates every figure, table row and number in the letter, and asserts the headline statistics |

## Reproduce

```bash
python -m pip install matplotlib
python paper/make_figures.py        # writes paper/figures/, fails if any quoted number drifts
python timesync-2026-09-09/analyze.py   # re-derives analysis/ from the raw transitions (needs numpy)
```

Rules used by the analysis: a pulse counts when its HIGH width is 40–65 ms; follower edges are matched one-to-one to reference edges within ±0.5 s; no outlier removal, glitch filtering or detrending. Quantiles use the nearest-sample rule, index round(p(n−1)).

## Scope and limits

- Offsets are electrical GPIO phase relative to the reference node. They are not optical exposure times.
- No record is longer than 60 s, so timestamp-wrap handling over longer runs is not covered here.
- Nine of fifteen planned campaign records finished before analyzer timeouts stopped the campaign. Short and failed attempts are kept but are not counted.
- `acquire.py` and `check_signal.py` drive physical hardware and are kept for provenance only.
- Some JSON logs contain private-subnet node addresses (192.168.137.x) from an isolated laptop hotspot.

Firmware and host software: <https://github.com/IEatCodeDaily/wireless-ir-mocap>.

## License

Data: CC BY 4.0. Code: MIT. See `LICENSE`.
