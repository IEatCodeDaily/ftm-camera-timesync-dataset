# Time-sync measurement, 8 September 2026

## Latest result — firmware epoch repair and hardware pulse generation

The later firmware experiments supersede the unresolved phase-jump hypotheses
in the historical sections below. Raw FTM timestamps wrap every
281.474976710656 seconds (48-bit picoseconds). Extending local and remote epochs
before fitting removed the large 50/425/475-ms phase jumps. A responder boot
nonce resets the fit after restart; coarse UDP epoch announcements select the
wrap cycle only, while fine synchronization remains FTM-based.

After the user reconnected node 3's probe, two loaded 60-second MCPWM captures
at 16 MS/s had 60 rising edges on all four channels. Across those trials,
relative GPIO offset SD was 0.48–0.70 us and worst absolute offset was 1.625 us
against node 2. These are externally measured relative output metrics, not
absolute UTC accuracy or a calibrated oscillator measurement. The sample
interval is 62.5 ns. Long 24 MS/s attempts suffered analyzer ReadTimeout.

The final firmware `e0ac899e423e2d93` is installed on nodes 0–3 and defaults to
MCPWM hardware edges. Task, high-priority task, esp_timer, GPTimer, paired
GPTimer, frozen FTM and hotspot TSF alternatives were also investigated.
This is a representative implemented comparison, not every conceivable design.

A final 60-second loaded capture after restarting node 2 alone also recorded
60 pulses on every channel: SD 0.578/0.582/0.563 us and mean offset
0.063/-0.392/-0.216 us for nodes 0/1/3. Worst absolute offset was 1.937 us.
Evidence: `test-output/timing-methods/20260908-174159/00-mcpwm-ftm/`.
All nodes passed final online/firmware/clock checks, with pulse modes OFF.

See the [experiment log](TIMESYNC_EXPERIMENT_LOG_2026-09-08.md) and
[complete method inventory](TIMESYNC_METHOD_RESULTS_2026-09-08.md) for failed
captures, firmware IDs, telemetry, screenshots, and raw evidence. A static-HIGH
control confirmed synthetic capture-start edges at exactly 128 us; raw data
and default metrics remain unfiltered.

## GPIO2 baseline (before rewiring)

All nodes were reachable on firmware `c22e614ca42e170d`. Nodes 0, 1 and 3 had
valid affine clock models; node 2 was the FTM responder. All pulse modes were
initially OFF. Arming explicitly enabled 1HZ on initiators 0, 1 and 3.

The physical analyzer reported `DeviceType.LOGIC`, ID `8BBBCD9E91E0E1D6`, with
Logic 2.4.46. Four individual identify captures at 24 MS/s confirmed that each
node's commanded activity appeared on its corresponding channel 0–3. Channel 1
had four clean identify pulses (~120 ms high). Channels 0, 2 and 3 also contained
tens of thousands of short transitions; their activity is not a clean timing
waveform. User reported analyzer GND connected directly to node 1 only, with
all nodes powered from the same USB power supply. Ground return integrity is
a hypothesis to test, not a proven cause.

24 MS/s completed short captures but two longer attempts returned USB
`ReadTimeout`. A continuous 60-second capture succeeded at 16 MS/s (62.5 ns
sample spacing). No digital glitch filter was applied.

| Baseline measurement | Node 1 / channel 1 |
|---|---:|
| Complete pulses | 60 |
| Period intervals | 59 |
| Mean period | 0.999968219 s |
| Period standard deviation | 20.601 us |
| Period minimum / maximum | 0.999855813 / 1.000079500 s |
| Period peak-to-peak | 223.687 us |
| Mean high width | 49.750 ms |

These are GPIO-output measurements relative to the analyzer's timebase.
The mean period's -31.781 us difference from one second is NOT an absolute
clock-accuracy measurement. Analyzer timebase error is not calibrated here.
Period jitter also includes firmware scheduling and GPIO edge-generation effects;
it is not an isolated estimate of FTM clock-model jitter.

Channels 0 and 3 had respectively 878,395 and 1,073,161 raw rising transitions,
with zero intact 40–65 ms high pulses. Pairwise offset, skew, jitter and drift
are therefore **unmeasured**, not zero and not a pass. No useful inter-node timing
can be inferred from this baseline. Node 2 is not expected to emit the current
model-gated 1HZ pulse because its responder clock model is invalid.

Evidence: `test-output/timesync-captures/20260908-160051-sync/` contains the raw
`.sal`, eight-channel CSV, acquisition settings and `timing-diagnostic.json`.
Earlier 2 MS/s baseline: `20260908-155551-sync`.
24 MS/s identify captures: `20260908-155729-identify1`,
`20260908-155753-identify0`, `20260908-155758-identify2`,
`20260908-155804-identify3` under the same capture directory.

## Dedicated pin and repeat procedure

The new configuration assigns timing output to GPIO41 and identify LED to GPIO2.
The image built successfully as `765db87164e3b783` (1,032,000 bytes), application
SHA256 `0e27664e8f5a16ba6d305259294ccaf5bd0d071ff8eff4c5838a8f007789122c`.
All four nodes subsequently passed OTA upload, reboot, changed-slot and live
identity/role/pin verification. Evidence:
`test-output/ota-fleet/1788858933478013900/nodeN/verified.json`.
Studio was rebuilt against this bundle and its UART bundle test passed. The
old UART watcher is paused during measurement; its image pins were updated so
an explicit future start selects the GPIO41 build.
`mode` reports both pins; `identify` must not interrupt the dedicated timing
output. Do not assume this configuration is installed until the live node's
build identity and mode response are verified.

With nodes powered off, connect each GPIO41 to its corresponding analyzer channel
and each board GND to a short common analyzer-ground connection. Preserve the
raw capture, record the firmware identity and confirm signal integrity before
interpreting sub-microsecond differences. Use 24 MS/s when a full capture works;
record any fallback explicitly. Sample spacing is not guaranteed measurement
accuracy. Saleae documents the original Logic's possible USB throughput limits:
https://new.saleae.com/support/troubleshooting/capture-and-recording-issues/device-not-able-to-keep-up

Run `prepare_timesync.py --expect 0 1 2 3 --measure 0 1 2 3 --action arm
--expected-build cd60c27a7f89bae0` before capture. `capture_timesync.py --seconds 60
--rate 24000000` saves channels 0–3 through the background API (use `--channels`
to choose another set).
`analyze_strobe_capture.py PATH/digital.csv` reports pulse count, raw glitches,
period/width statistics, pairwise signed offset, offset standard deviation,
peak-to-peak, p95 absolute offset, relative drift and detrended jitter. Its
width qualification is explicit and results remain diagnostic when glitches
exist. Pairwise matching only accepts edges within 250 ms and never reuses an
edge. Absolute accuracy requires an independent calibrated reference.

For the final clean trace, also run the existing strict `analyze_timesync.py`
on separate channel CSVs for nodes 0, 1, 2 and 3. It requires at least 30 edges,
valid cadence and the project's 1 ms criterion; absent data cannot pass.

## Clean GPIO41 trace exposed the clock-domain error

Capture `20260908-161843-sync` completed at 16 MS/s after rewiring and installing
`765db87164e3b783`. Channels 0, 1 and 3 each had 60 complete, intact pulses and
no rejected high pulses. Node 1's GPIO2 identify command was also exercised
during this record; its GPIO41 output continued pulsing. This is a diagnostic
record with that command activity, not a fully idle baseline.

| Pair (second minus first) | Mean offset | Offset standard deviation |
|---|---:|---:|
| 0 to 1 | +3960.141 us | 68.006 us |
| 0 to 3 | -3578.879 us | 83.899 us |
| 1 to 3 | -7539.020 us | 48.845 us |

The strict raw-edge analyzer also returned FAIL for the 1 ms criterion. These
offsets must not be hidden by detrending or by quoting FTM-fit residuals instead.

Inspection found that the 1HZ path fed `esp_timer` timestamps into a model fit to
Wi-Fi FTM MAC timestamps. Their epochs differ. The corrected implementation
reads `esp_wifi_internal_get_mac_clock_time()`, brackets it with `esp_timer`
reads, extends its 32-bit counter near a recent timestamp, and uses one model
snapshot for forward/inverse conversion. The responder emits its own MAC-clock
second without fabricating an initiator model; followers map that reference.
The new `mode` diagnostics expose the clock source, readiness, role, paired
timestamps, epoch offset and read-span estimate.

The installed IDF header documents MAC time in microseconds, 32-bit rollover
at about 71 minutes, and the requirement to disable modem/light sleep.
`wifi_setup.c` already selects `WIFI_PS_NONE`. Host C tests cover extension on
both sides of rollover and later epochs. Preflight tests now allow a ready
responder reference without a fake model and reject an invalid clock pairing.
The clock-domain-corrected image was then tested on the physical analyzer below.

## Four-node result with the MAC-clock correction

Build `cd60c27a7f89bae0`, application SHA256
`4469b90516b7e4bf56033850593aa76c53eecc1bd1706d7cfde556e9954c4aec`, passed
OTA verification on nodes 0–3. Evidence is under
`test-output/ota-fleet/1788859620928177200/`. Node 2 now emits the MAC reference
pulse on GPIO41. GPIO2 remains the independent identify LED. Live clock-pair
read spans were 1 us in the arming preflight; this is diagnostic pairing latency,
not proof of 1 us absolute accuracy.

The continuous 60-second record `20260908-162922-sync` completed at **16 MS/s**.
All four channels had 60 complete pulses, no rejected high pulses, and no
unmatched edges in the diagnostic pairwise comparison. No outliers were removed.
Offsets below are **follower minus responder node 2**, in microseconds; jitter
is the population standard deviation of the measured offsets.

| Follower | Mean offset (us) | Jitter SD (us) | p95 absolute offset (us) | Maximum absolute offset (us) |
|---|---:|---:|---:|---:|
| Node 0 | -1.033 | 12.000 | 1.938 | 92.313 |
| Node 1 | +58.081 | 423.837 | 9.875 | 3302.562 |
| Node 3 | -1.099 | 12.133 | 1.313 | 94.063 |

The raw-edge strict analyzer also returned **FAIL** for the <1 ms skew
criterion. Its common-window policy excludes boundary pairs, so its n=58
summary differs slightly from the complete-pulse diagnostic's n=60 summary.
A parser bug that treated an initially HIGH sample as an observed rising edge
was fixed and regression-tested; all 13 Python tests now pass, along with the
host C rollover tests and Studio's exact UART bundle test.

A separate five-second **24 MS/s** capture (`20260908-162856-sync`) contained
five complete pulses per channel. Node 0's maximum absolute offset was 0.458 us,
node 1's was 1.708 us, but node 3 had a **13,760.375 us** outlier. This short
record is not sufficient to establish long-run jitter and must not be omitted
when discussing worst observed performance. Multiple attempted 24 MS/s continuous
records failed with USB ReadTimeout; no successful 60-second 24 MS/s result is
claimed. 16 MS/s is the highest demonstrated continuous rate in this session.

In this first capture the fixed millisecond-scale inter-node bias was absent,
but subsequent rearming tests below show that this improvement is not stable.
Rare large timing excursions also remain. The GPIO edge is still generated by a priority-1 FreeRTOS
task after an esp_timer wait; these measurements include task/interrupt/edge
generation latency as well as clock-model error. Similar excursions in period
and width are consistent with scheduling delays, but this record does not
independently isolate their cause. The responder's physical pulse has jitter
too, so its delay contributes to every follower-minus-reference measurement.

This establishes relative **GPIO output timing**, not calibrated absolute clock
accuracy, nanosecond FTM accuracy, or camera exposure synchronization. FTM-fit
residuals must not be substituted for these external measurements. The regression
slope in the diagnostic JSON is the observed offset trend, affected by outliers;
it is not an independently measured oscillator drift.

Artifacts in `test-output/timesync-captures/20260908-162922-sync/`:

- `capture.sal`, `digital.csv`, `capture.json`: original acquisition and settings.
- `timing-diagnostic.json`: median/RMS/SD/percentiles/extrema, periods, widths,
  all six pairwise offsets, observed drift fits and detrended jitter.
- `strict-report.json`: the project's 1 ms criterion result.
- `reference-offsets.csv`, `reference-offsets.png`, `reference-offsets.svg`:
  responder-relative offsets and plots with outliers retained.

At completion, all test modes were set OFF. Studio was rebuilt against
`cd60c27a7f89bae0` and restarted in the background. The UART watcher remains
paused during the experiment, with its pinned image updated to the same build.

## Retest after cable movement: reference phase is not stable

The user moved the cables and requested another measurement. Firmware remained
`cd60c27a7f89bae0`. Capture `20260908-163915-sync` recorded 60 seconds at
16 MS/s: all four channels had 60 complete pulses and zero rejected high pulses.
Another 24 MS/s continuous attempt failed with ReadTimeout.

| Follower | Mean offset from node 2 (us) | Offset jitter SD (us) |
|---|---:|---:|
| Node 0 | -50040.679 | 59.335 |
| Node 1 | -50043.226 | 51.749 |
| Node 3 | -50050.200 | 30.735 |

Node 1's SD decreased from 423.837 to 51.749 us, but all followers now lead
node 2 by about 50.044 ms. This fails the 1 ms skew criterion. Reduced SD
does not mean improved reference accuracy, and cable movement cannot be
identified as the cause from these records alone.

In the earlier capture, node 1's median offset was only +0.531 us. Its SD was
dominated by a +3302.562 us excursion and a +254.876 us excursion; 56 of its
60 offsets were within +/-2 us. A separate roughly -93 us excursion appeared
on all followers together, consistent with a delayed reference edge. Software
scheduling is a candidate cause, but deadline-versus-actual-edge telemetry is
needed to distinguish it from clock-model or reference-clock errors.

After stopping and rearming, capture `20260908-164334-sync` recorded ten clean
pulses per channel over ten seconds at 16 MS/s. The first node 2 edge was at
0.349536063 s, while the first follower edges were at about 0.774453 s:
approximately +424.917 ms phase difference. Periodic pulses alone do not
identify which integer-second cycle corresponds to which. The diagnostic
analyzer's +/-250 ms matching window therefore reports no matched reference
pairs; this is not an absence of pulses.

The reference-relative phase change between rearming runs is unresolved.
These records do not establish stable FTM reference accuracy. Next diagnosis
should independently verify channel-to-node mapping and instrument scheduled
versus actual edge times and the FTM-to-reference clock relationship.
All four test modes were stopped afterward (preflight `20260908-164440-293188`).
Raw captures and diagnostic JSON remain in the timestamped capture folders.

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
