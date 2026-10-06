# Firmware-only synchronization experiment log — 8 September 2026

All times are laptop local time (Asia/Jakarta). Wiring remains unchanged:
GPIO41 on nodes 0–3 to analyzer D0–D3; GPIO2 is the identify LED.
Node 2 is the FTM responder. No hardware changes are part of this campaign.

## Evidence policy

Each test retains the raw Saleae `.sal` and transition CSV when acquisition
succeeds, firmware ID, configuration/arming/stopping responses, post-stop
per-pulse telemetry, metrics, and any acquisition error. No outliers are removed.
Offsets are GPIO timing relative to node 2, not calibrated absolute accuracy.
Nearest-phase statistics do not identify integer-second correspondence.

## Earlier experiments

Detailed prior history and measurements are in
[the measurement report](TIMESYNC_MEASUREMENT_2026-09-08.md): GPIO2 signal
checks, GPIO41 separation, OTA receiver repair, initial MAC-clock conversion,
and the cable-movement retests. The latter showed common reference phase errors
of approximately -50 ms and +425 ms; their cause remains unresolved.

## 16:59 — Instrumentation and restart fix

Built and installed `34e1a3df22c26af8` on all four nodes using verified OTA.
Evidence: `test-output/ota-fleet/1788861580427074800/`.

Changes:
- Stop wakes the strobe task and waits for complete teardown before permitting
  another start, replacing the previous fixed 600 ms timeout.
- Added volatile `timing config BACKEND SOURCE` settings. Backends: original
  core-0 priority-1 task, core-1 priority-20 task, esp_timer task callback,
  and GPTimer interrupt callback. Sources: live FTM, frozen FTM fit, hotspot
  TSF, and unsynchronized local MAC control.
- Added a 32-pulse ring buffer with target second, paired clocks, deadline,
  timestamp immediately before the GPIO write, model revision/slope/offset,
  and clock-read span. It is dumped only after stopping.
- GPTimer still toggles GPIO in an interrupt; it is not a hardware comparator
  output. Telemetry is not a substitute for the external analyzer.

Build passed; existing Python analyzer/preflight tests: 13 passed.

## 17:01 — Channel identity check

Only node 2 was armed by software. At 16 MS/s over four seconds, D2 had four
pulses and D0/D1/D3 had zero. This independently confirms the reference channel
without touching cables. Evidence: `test-output/timing-methods/20260908-170133/`.
An earlier attempt (`20260908-170100`) acquired but failed export because a
relative output path was resolved by Logic; fixed the runner to use absolute
paths and retained the failed attempt's error and telemetry.

## 17:02 onward — First method comparison

Evidence root: `test-output/timing-methods/20260908-170156/`.
Each planned capture is 25 seconds at 16 MS/s on D0–D3. All nodes are stopped
between methods; configuration, raw captures, and results are saved per method.

- `task:ftm`: 25 pulses on each channel. Node 0/1/3 offset SD respectively
  0.687 / 1.581 / 0.530 us; largest absolute offset 5.375 us. A short successful
  capture does not explain or rule out the earlier phase jumps.
- `high:ftm`: acquisition failed with analyzer ReadTimeout; no timing verdict.
- `esp:ftm`: 25 pulses/channel. Offset SD 23.434 / 28.741 / 22.136 us. Buffered
  deadline lateness reached 143 us on node 2. This callback path is worse in
  this capture than the instrumented task baseline.
- Remaining results are recorded in per-method `result.json` as they complete;
  the campaign summary will be updated after the comparisons.

## Screenshot documentation

The user requested periodic Logic-window screenshots during this campaign.
The requested CUA driver was checked: it currently exposes no native apps,
only the Codex in-app browser. Logic's local MCP API also has no screenshot
method. No foreground/native-input fallback has been used. Raw `.sal` captures
and numerical evidence remain available; window screenshots are blocked until
the CUA driver exposes the Logic window.

## 17:09 — Cua Driver installed and screenshots verified

The earlier check addressed the bundled Codex CUA interface, not the separate
trycua Cua Driver requested by the user. Installed official Cua Driver 0.24.0;
daemon is running and logon autostart is registered. Verified background
Logic-window capture using its CLI, without foreground activation or mouse
input. Screenshot: `test-output/timing-screenshots/20260908-1709-driver-verification.png`.
The test runner now takes screenshots during acquisition and after export,
before closing the capture. UI Automation enumeration reported a provider
timeout; screenshot-only capture bypasses that walk and succeeds.

## 17:11 — Hardware output and robust timer pairing

Built and OTA-verified `8dc05893b49ecfab` on nodes 0–3. Binary, ELF and strobe
source are archived in `test-output/timing-firmware/8dc05893b49ecfab/`.
OTA evidence: `test-output/ota-fleet/1788862169472275200/`.

- Added `gptpair`: repeats GPTimer/esp_timer pairing and uses the narrowest
  bracket, rejecting reads wider than 3 us. The original single-read GPTimer
  experiment had an early pulse and negative deadline lateness, consistent
  with pairing error; repeated pairing tests this explanation.
- Added `mcpwm`: hardware compare events generate both edges, with 50 ms high
  width, on unchanged GPIO41. A one-shot 1 MHz timer is mapped to esp_timer
  with narrowest-bracket reads. This experimental S3 implementation exclusively
  owns MCPWM group 1 timer 0; adding another owner requires revisiting it.
  Its recorded `fired_us` is ISR callback arrival AFTER the hardware edge.
- Repeating high-priority task, robust GPTimer, and MCPWM twice each at
  25 seconds, 16 MS/s. Evidence: `test-output/timing-methods/20260908-171055/`.

The generated [complete results inventory](TIMESYNC_METHOD_RESULTS_2026-09-08.md)
includes every completed/error run and links to raw evidence.

Screenshot caveat: Cua Driver window capture works, but Logic's main window
currently shows its connected/start screen while automation captures run.
Background Ctrl+O returned `background_unavailable` for the Electron window;
no foreground fallback was used. Such screenshots document application state,
not the waveform. Raw captures remain the timing evidence.

## 17:16 — Root cause of the large phase jumps reproduced

In `20260908-171431/00-task-ftm`, a 60-second capture under network load crossed
the remote timestamp rollover. Telemetry shows follower target seconds reverting
to 13–45 while the responder remains near second 294–326. The fit offset changes
by approximately -281,474,976.711 us. Subsequent high-priority/GPTimer tests keep
approximately +474,961 us phase error despite small deadline lateness.

The 48-bit picosecond period is 281.474976710656 seconds. One, two, and three
rollovers give nearest-second phase errors approximately +474.977, -50.047,
and +424.930 ms. Accounting for the analyzer-observed clock rate, these agree
with both the new reproduction and the earlier cable/rearm observations.
The large phase jumps are therefore explained by missing FTM timestamp epoch
extension; they are not fixed by changing the GPIO backend.

Repair in progress:
- Extend both remote and local FTM timestamps before RTT calculation and fitting.
- Responder broadcasts an additional CRC-protected type-4 coarse MAC epoch with
  its existing boot nonce/BSSID; original presence/announce/command packets are
  unchanged. The coarse epoch only selects the 281-second cycle. Network packet
  latency is not used to estimate fine offset or drift.
- Require a fresh matching epoch; reset the fit when responder identity/boot
  changes. Preserve original raw timestamps in frame telemetry.
- Host tests cover forward/backward wrap, multiple wraps, fresh boot, and RTT
  with independent local/remote epochs; these pass.

## 17:22 — Rollover repair installed; repeated loaded validation

Build `33a74b5c18b078e9` passed OTA verification on all nodes. Evidence:
`test-output/ota-fleet/1788862873482175600/`. A software preflight confirmed
all four nodes online, followers fitted, and GPIO41/GPIO2 configuration correct.
The default 1 Hz backend is now `gptpair`; MCPWM remains experimental.

Running three alternating 60-second GPTimer/MCPWM trials with up to five
health requests per second per node. This campaign spans a real 48-bit FTM
rollover. Evidence: `test-output/timing-methods/20260908-172251/`.

The first repaired captures show channel 3 flat HIGH despite node 3 recording
regular pulse deadlines/callbacks. Those runs are INCONCLUSIVE for the full
four-node comparison, not a zero-jitter success. Channels 0–2 remain measurable.
Adding a software-only static GPIO41 low/high/read diagnostic to distinguish
pad output from the analyzer input. No probes or grounds have been moved.

The Cua Driver skill pack was also installed and linked into the local Codex
skills directory. Background screenshots are retained per capture. A one-pixel
resize/readback/restore through the driver refreshed the Logic window frame but
its occluded Electron client area remained blank; no foreground input was used.

## 17:30 — User reconnected node 3 probe; four-channel repeat

The user confirmed that the node 3 probe had been disconnected and reconnected it.
The preceding flat-HIGH captures and the reconnection transient are inconclusive
for full-fleet timing. Their raw recordings remain intact.

Two 60-second, 16 MS/s MCPWM/FTM captures under network health polling each
contained 60 rising edges on every channel. Evidence:
`test-output/timing-methods/20260908-173016/{00,01}-mcpwm-ftm/`.

| Trial | Mean offset node 0 / 1 / 3 (us) | Population SD node 0 / 1 / 3 (us) | Worst absolute offset (us) |
| --- | --- | --- | --- |
| 00 | -0.079 / -0.310 / -0.463 | 0.704 / 0.611 / 0.522 | 1.562 |
| 01 | -0.132 / -0.235 / -0.523 | 0.608 / 0.635 / 0.481 | 1.625 |

These runs occurred after two real 48-bit FTM rollover periods without reboot.
Two GPTimer attempts in this campaign failed with analyzer ReadTimeout; they
provide no timing verdict. The standalone `00-mcpwm-ftm/offsets.png` plot was
rendered and inspected; all offsets are retained.

## 17:39 — Final default and acquisition-start control experiment

Build `e0ac899e423e2d93` installed and verified via OTA on all four nodes:
`test-output/ota-fleet/1788863878248252500/`. It retains the validated epoch repair,
selects MCPWM as the default, and adds an OFF-only `timing pin low|high|read`
diagnostic. GPIO41 remains strobe; GPIO2 remains identify LED. Firmware/source
archive: `test-output/timing-firmware/e0ac899e423e2d93/`.

The static control experiment held all four GPIO41 outputs LOW, then HIGH,
before separate 3-second recordings. LOW recorded no edges. HIGH pad readback
was 1 before acquisition, but all channels exported initial 0 then a rising
transition at exactly 0.000128 seconds. Thus this acquisition path produces a
startup transition for an already-HIGH input. Evidence:
`test-output/timing-static/20260908-173946/result.json` and raw `.sal`/CSV files.
All pads were restored LOW. No hardware changes were made by the agent.

The analyzer supports an explicitly requested startup guard in a separate
diagnostic report; default reports and raw counts remain unfiltered. This
artifact explains the 128-us initial edges, not later in-recording outliers.

Host MAC/FTM epoch and independent-clock RTT tests pass; 14 Python timing-tool
tests pass. The paused UART watcher now pins the final build and application hash.

## 17:43 — Responder-only restart recovery verified externally

Sent `restart` only to node 2. The console command timed out waiting for its
prompt, as the connection closed during reboot; subsequent mode telemetry
confirmed reset uptime. Followers remained running and refitted to the new
reference epoch (approximately -101 seconds offset due to differing uptimes).

First 60-second acquisition (`20260908-174044`) failed with analyzer
ReadTimeout and has no timing verdict. Retry
`test-output/timing-methods/20260908-174159/00-mcpwm-ftm/` succeeded at 16 MS/s,
with 60 clean rising edges on each channel while health polling continued.
No startup guard or outlier removal was applied.

| Node versus node 2 | Mean offset (us) | SD (us) | RMS (us) | Peak-to-peak (us) | Worst absolute (us) | Relative drift (ppm) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0.063 | 0.578 | 0.581 | 2.749 | 1.937 | -0.00019 |
| 1 | -0.392 | 0.582 | 0.701 | 3.125 | 1.812 | -0.00161 |
| 3 | -0.216 | 0.563 | 0.603 | 2.751 | 1.438 | 0.00401 |

These short-record drift estimates are relative GPIO edge trends, not
calibrated oscillator accuracy. All pairwise offsets, period/width statistics,
percentiles and detrended jitter are in `result.json`. The rendered
`offsets.png`/SVG and offset CSV are saved alongside raw `.sal` and digital CSV.
The PNG was inspected. Cua Driver window screenshots accompany the capture;
they may show a blank occluded Logic client, rather than the API recording.

All pulse modes were stopped after the trial. Studio's embedded firmware bundle
was rebuilt against the final application and the server restarted in the
background; its UART validation test passed. Final preflight at 17:44 confirmed
all four nodes reachable on `e0ac899e423e2d93`, all pulse clocks ready, and all
modes OFF. Evidence: `test-output/timing-preflight/20260908-174446-698101/`.

## 17:48 onward — Camera pipeline load tests

See [pipeline test report](PIPELINE_TIMESYNC_2026-09-08.md) for the full
tracking baseline, image-preview failure, user-assisted reset and concurrent
clock diagnostic. The empty scene is intentional. Initial tracking generated
14,010 verified MCAP records with zero writer loss; camera preview subsequently
hung command access on nodes 0 and 2. The user reset all four nodes. Raw tests
and per-run Cua Driver screenshots are under `test-output/pipeline/`.

### 18:55 pipeline/recovery update

See [loaded pipeline and recovery report](PIPELINE_TIMESYNC_2026-09-08.md).
The latest three-node detection run recorded 10,399 packets with zero sequence
gaps. Preview exposed the ten-socket limit; a 20-socket fix is built as
`662952cc2cfa3bf0` but has not been uploaded because the host Wi-Fi radio is off.
Latest installed image remains `31359c922f9f1b88`. The analyzer now fails even
a one-channel 4 MS/s diagnostic; no new timing result can be claimed.
Background Cua screenshots, raw reports, and unsuccessful trials are preserved.


## 20:10 update: fleet online and FPS trial complete

Wi-Fi connectivity was restored and all four nodes passed OTA/readback on
`a12710b93010e136`, including the 20-socket recovery budget. Final tracking
measured 53.479/52.783/50.982/52.793 FPS on nodes 0/1/2/3 with zero sequence
gaps across 14,840 recorded packets and no recovery increments. Four-node
preview also completed without request failures or resets. Studio was rebuilt
with the final image, all 37 server tests passed, and the hidden server was
restarted. Final preflight `20260908-201049-831080` verified all four online,
matching firmware and mode OFF, with no errors. Analyzer USB ReadTimeout
remains unresolved: no new independent clock-jitter result is claimed.
See [the FPS experiment log](TRACKING_FPS_2026-09-08.md) for accepted settings,
rejected DMA trials, per-node results and evidence locations.


## 22:20 pipeline optimization and resolution qualification

The [resolution benchmark](RESOLUTION_BENCHMARK_2026-09-08.md) measured all exposed modes on baseline a12710b93010e136. The [architecture review](ARCHITECTURE_REVIEW_2026-09-08.md) identified bounded-window, ray-validity, calibration and clock-domain gaps. User then authorized pipeline implementation and required UDP preview/video.

The [optimization log](PIPELINE_OPTIMIZATION_2026-09-08.md) records primary online references, implementation decisions, canary runs and fleet evidence. Firmware 5a2fa4c4c1f0571a is installed on all four nodes. OTA readback was briefly delayed by my Studio rebuild/restart; uploads were not repeated and all identities/pins were verified afterward (ota-fleet/1788880434928142300).

Fleet matrix 20260908-221552: initial QVGA saw node 3 reset with ESP_RST_BROWNOUT (9), followed by a node 2 command timeout before VGA could start. These are failed trials. HD and SXGA subsequently completed without resets. The analyzer remains unavailable for valid new waveforms; Cua Driver screenshots are saved at each test start/end without foreground interaction. Complete metrics and follow-up results are appended to the optimization log.


## 22:33 final optimization state

All exposed resolutions were exercised on all four nodes. Repeated tracking rates: QVGA 51.60-53.51 FPS, VGA 17.21-17.27, HD 17.75-17.79, SXGA 6.61-6.63 and FHD 8.21-8.25. Every JPEG resolution completed over UDP without request errors; raw QVGA UDP also passed transport/health checks. These are throughput results with a dark/no-target scene, not marker accuracy. The earlier brownout and command timeout remain failures, not erased by the repeats.

Node 1 sensor-generated bars passed both JPEG and exact Y8 UDP checks; pattern-off optical images remained dark. Diagnostic-only firmware fdb0d3b4ab514a2d is now installed on all four nodes and embedded in rebuilt Studio. Patterns are off, all modes OFF, and final preflight 20260908-223331-958087 passed software readiness. Clock readiness after OTA restart does not prove long-uptime recovery. Independent exposure/jitter remains unmeasured because the analyzer capture fails.

Full per-node tables, source references, screenshots, run IDs, code changes and validation are in [the optimization log](PIPELINE_OPTIMIZATION_2026-09-08.md).
