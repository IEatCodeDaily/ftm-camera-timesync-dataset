# Timesync: visual summary

Start with `timesync_visual_guide.pdf`. It contains the five-source comparison, an explanation of clock offset and speed correction, conceptual diagrams, the supplied setup photo, an actual Logic screenshot and the six output-backend comparisons.

**Best-supported choice for this implementation: live FTM + MCPWM.** The new clean 60-second FTM trial had a largest observed relative GPIO error of 1.688 microseconds, with no missed cycles. Three separate historical repaired MCPWM/FTM trials were below 1.937 microseconds. This is an engineering recommendation from limited observations, not a guaranteed bound or a statistically established universal winner. Frozen FTM had similarly small delivered-pulse errors over short records, but does not keep updating its model and had delivery gaps in these trials.

The primary chart uses nine completed 60-second trials on the same firmware. FTM has one trial, the other sources two each. Longer acquisition failures prevented the remaining repetitions. Its horizontal axis is logarithmic, so each tick represents a tenfold change. Gaps are intervals longer than 1.5 seconds across all four channels. No-sync phase depends on boot epochs; it is not a fixed oscillator property. Simple NTP-style means the private latest-offset estimator tested here, not a complete RFC NTP/SNTP client.

## How it works

Each node's clock can start at a different value and tick at a slightly different rate. Timestamped exchanges provide observations linking the local clock to a reference. FTM uses radio-event timing to estimate an offset and a clock-speed correction. The model converts local time to reference time. Each node then schedules the same reference second, and MCPWM generates the GPIO pulse. The analyzer measures the actual resulting edge differences. This does not measure camera exposure alignment.

The four-timestamp diagram is a conceptual request/reply exchange, not a literal FTM packet trace. The simple software estimator uses offset = [(t2 - t1) + (t3 - t4)] / 2; delay asymmetry can bias it. See [RFC 5905, Section 8](https://www.rfc-editor.org/rfc/rfc5905.html#section-8), [Espressif's FTM example](https://github.com/espressif/esp-idf/blob/v5.5.1/examples/wifi/ftm/README.md), and the implementation/evidence discussion in the main report.

## Reuse the figures

- `method_comparison.png` / `.svg`: all five clock sources, with repetition and gap counts.
- `ftm_overview.png` / `.svg`: reference, followers, clock mapping and GPIO scheduling.
- `timestamp_exchange.png` / `.svg`: conceptual timing-message exchange.
- `logic-capture.png`: actual Logic window showing the archived clean FTM capture. The disconnected label reflects viewing an archived recording. D4-D7 are unused; D0-D3 are measured.
- `recorded_edge_zoom.png` / `.svg`: one event reconstructed from the same raw transition CSV, explicitly not a screenshot or simulated measurement.
- `backend_comparison.png` / `.svg`: all six output backends in two distinct same-build exploratory blocks. Do not pool the two firmware generations into a fair six-way ranking.
- `setup.jpg`: the supplied photograph, copied unchanged. Physical left-to-right node IDs and exact board manufacturer/revision remain unidentified.
- `figure_data.json`: plotted values and source-run paths.

## Apparatus clarification from the photograph

The physical analyzer is marked 24 MHz / 8CH. The software reported a Logic device identity; that is not verification of the physical manufacturer's identity. The earlier report's “original Saleae Logic” wording describes a software device classification and should not be taken as verified manufacturer attribution. The successful comparison sample rate remains 16 MS/s regardless of the case marking.

## Provenance and reproduction

The chart inputs are retained from `../analysis/run_metrics.json`; the edge plot uses `../new-acquisition/20260909-065110-campaign/00-00-mcpwm-ftm/digital.csv`. The Logic screenshot was captured after loading the corresponding saved `.sal`; no new measurement was performed for this visual summary. `make_visuals.py` regenerates figures/PDF when run inside the original report folder, using Python, NumPy, Matplotlib, Pillow and ReportLab with Windows Arial fonts. PDF pages were rendered and visually checked. `visuals_manifest.json` records delivered-file hashes.

The original full evidence archive remains unchanged. This supplement adds easier-to-read explanations and the new photo-based apparatus clarification.
