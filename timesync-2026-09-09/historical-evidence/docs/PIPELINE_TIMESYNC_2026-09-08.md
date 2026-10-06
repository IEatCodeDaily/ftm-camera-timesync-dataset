# Camera pipeline and timing load test — 8 September 2026

The user requested camera streaming, the detection pipeline and timing tests
with the existing hardware. Cameras point at an empty scene, so zero markers
is expected. This evaluates execution and transport, not marker localization,
3D reconstruction accuracy or optical exposure alignment.

## Existing tracking pipeline

Studio starts all four nodes with `mode live 0 --nomesh`: free-running native
QVGA Y8 capture, firmware centroid detection, IRP1 UDP, host validation and MCAP.
It does not stream images concurrently. The separate image preview owns the
camera in a different mode. GPIO41 ordinarily emits a short software pulse on
frame grab; that is not the 1 Hz hardware clock pulse and is not exposure start.

Baseline evidence: `test-output/pipeline/20260908-174832-tracking/`, firmware
`e0ac899e423e2d93`. A 60-second observation plus startup/stop tails produced the
following full MCAP results. All 14,010 records were read back successfully;
writer queue drops and write errors were zero. All node detector selftests passed.

| Node | MCAP records | Received FPS | Sequence gaps | Missing fraction | Mean detector time (ms) | Maximum (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 2771 | 35.34 | 0 | 0% | 1.912 | 3.491 |
| 1 | 3854 | 50.13 | 1 | 0.026% | 1.908 | 4.039 |
| 2 | 3690 | 48.93 | 9 | 0.243% | 1.978 | 3.803 |
| 3 | 3695 | 50.15 | 41 | 1.097% | 1.921 | 4.160 |

No detected markers, capture failures or detector overflows were reported.
Node 3 reported 38 transmit drops at the stats snapshot. Sequence gaps include
loss before host acceptance and cannot all be attributed to the network alone.
The MCAP spans start/stop tails, so record count divided by 60 is not FPS.
The WebSocket observation closes before MCAP finalization and contains fewer
records; it is not the authoritative recording count.

The 15-second analyzer capture recorded 610/751/748/761 frame-grab edges on
channels 0/1/2/3. Frame-grab periods have millisecond variability and are
free-running; nearest-edge phase would not establish synchronization.

## Image-preview test and recovery

Two initial configuration attempts requested JPEG over TCP/Wi-Fi and were
rejected by the existing grayscale-only API contract. These are harness
configuration errors, not camera failures. Their result files are retained.

The corrected QVGA grayscale Wi-Fi preview run
`test-output/pipeline/20260908-175201-preview/` delivered images from all four
nodes, but did not pass sustained operation. Frame counts were 9/39/1/30 for
nodes 0/1/2/3. Node 2 stalled after one frame; node 0 also had frame errors.
A node 1 stats command returned connection closed. Nodes 0 and 2 subsequently
failed command-port access and direct network restart attempts. No camera UART
ports were connected. The user reset all four nodes to recover them.

Raw initial images, per-request headers, errors, analyzer capture, WebSocket
events and recovery attempt are retained. This run does not establish a root
cause for the hang. Preview success must not be inferred from initial images.

## Clock diagnostic while detection runs

Diagnostic build `243f35ae79212cc1` adds `timing monitor on|off` after a camera
mode starts. While enabled, GPIO41 exclusively emits the existing MCPWM/FTM
1 Hz clock pulse; capture and detection continue, and per-frame GPIO pulses
are suppressed. Stopping/changing camera mode tears down the monitor. OFF-only
configuration and standalone 1 Hz behavior remain available.

This diagnostic measures clock-output timing under the real computational and
UDP load. It does not convert free-running frames into synchronized exposures.
It also replaces the ordinary strobe waveform while active and is a test mode,
not a production illumination setting.

The separate paced-live code still applies a MAC-domain FTM model directly to
`esp_timer` and requires a valid fit even on the responder. Studio uses uncapped
live mode, so this defect does not block this throughput test, but paced live
must be repaired and separately validated before claiming synchronized capture.

The loaded 60-second monitor capture on build `243f35ae79212cc1` completed:
`test-output/pipeline/20260908-175800-tracking/`. Each channel had 60 matched
50-ms clock pulses after the explicit 40–65 ms width gate. Raw edge counts were
60/60/157/85 on channels 0/1/2/3. Channels 2 and 3 also contained 97 and 25
one-sample (62.5 ns) HIGH glitches. Therefore the signal-clean check FAILS;
the following qualified-pulse metrics are not an unfiltered all-edge pass.
Detection continued near 49–50 FPS on all four nodes. `extra-edges.json` and
the raw-edge plot retain the glitches; their origin is not yet established.

| Node versus node 2 | Mean offset (us) | SD jitter (us) | Worst absolute (us) |
| --- | ---: | ---: | ---: |
| 0 | 0.230 | 0.900 | 2.063 |
| 1 | 0.875 | 0.881 | 2.750 |
| 3 | 0.325 | 0.723 | 1.562 |

This is modestly worse than the camera-OFF reference (roughly 0.56–0.58 us SD,
1.94 us worst), while remaining below 3 us worst in this recording. These are
finite-run relative output observations, not a guaranteed timing bound.

## Recovery implementation and validation

The user requested automatic recovery and suggested Rust for memory safety.
No memory-corruption cause has been established. A complete rewrite would still
depend on C camera/Wi-Fi drivers; current work targets observed availability
failure and adds evidence for future fault diagnosis.

Changes under validation:

- Independent 250-ms supervisor checks 30-second command/mode deadlines,
  20-second active TCP stream progress and 20-second uncapped live-frame progress.
- A stalled operation records a fault code/count in RTC memory and restarts.
  After this or a watchdog/panic reset, automatic camera boot mode is suppressed;
  the node rejoins Wi-Fi in OFF mode for inspection and explicit host restart.
- Task watchdog starvation now causes panic/reboot instead of warnings only;
  the existing interrupt watchdog remains enabled.
- TCP and USB CDC commands use private per-task stdout FILE streams instead
  of reopening the shared UART FILE while other tasks log through it.
- Health now reports actual responder role, reset reason, recovery count/fault,
  safe-boot state, uptime, and free/minimum internal heap. RTC evidence survives
  software reboot, not removal of power; host run logs persist on disk.

Fault codes: 1 command deadline; 2 mode transition; 3 active TCP stream;
4 deliberate test injection; 5 uncapped live capture stopped progressing.
No automatic return to the failed camera mode is attempted, avoiding reboot
loops. A healthy node sitting OFF without a client is not a timeout failure.

Recovery build `39fc477fe7b05766` compiled successfully. Canary node 0 accepted
OTA, but the original single-shot verification timed out during network readback;
later health and direct console readback confirmed the expected image and slot.
The OTA verifier now retries read-only postboot checks within a bounded window.

`test-output/recovery/20260908-181029/result.json` verifies the intentional
fault experiment on node 0. A cancelled 1-second lease caused no restart.
`recovery test stall` deliberately blocked the command task with a 2.5-second
deadline. The node automatically rebooted and was verified online after 9.063
seconds, on the same firmware, recovery count increased from 0 to 1, fault=4,
safe_boot=true and mode OFF. No physical reset was used. The first harness
attempt encountered an HTTP command error and is retained separately.

### Reproduced preview failure after the first recovery change

`test-output/pipeline/20260908-181524-preview/` on `39fc477fe7b05766` still
failed: unique frames 14/34/1/44 for nodes 0/1/2/3; node 2 again stalled after
one image. A node 0 command closed unexpectedly; node 2 health subsequently
became unreachable. Other nodes' recovery counters did not increase. Therefore
the injected-stall PASS does not establish recovery from this real failure.
The private stdout change did not fix the complete image-stream problem.

Build `31359c922f9f1b88` extends supervision to the entire accepted command
connection, including banner/receive/close, and adds an independent loopback
control-service probe. Probes skip active commands and OTA. A socket operation
blocked for 10 seconds, or three consecutive failed idle service probes,
causes a recorded fault 6 and safe reboot. Per-client UART log flooding was
removed. This extension is pending on-device qualification and fleet deployment.
The user was asked to attach node 2's UART and reset it for fault diagnosis;
network-only recovery was unavailable, and no UART was enumerated at that point.

The user could not connect UART, reset node 2 and reported fixing its USB power.
Subsequent trials must therefore be compared with that setup change disclosed;
an improved outcome cannot be attributed to firmware changes alone. No UART
fault trace is available. A read-only UART logger was prepared and then stopped
when the user confirmed UART was unavailable.

The extended build passed the on-device cancelled-deadline and injected-stall
checks on node 1 (`test-output/recovery/20260908-182310/result.json`): no false
reset after cancellation, and automatic fault-4 recovery to OFF after 9.703 s.
Nodes 1, 2 and 3 passed OTA identity/role/pin readback. Node 0's upload connection
reset after commit, but later health confirmed `31359c922f9f1b88` running in
the opposite OTA slot. The original transport failure remains in its log.
Studio was rebuilt against this same canonical firmware and restarted hidden;
the embedded UART-bundle validation test passed. The UART watcher pins were
updated, but the watcher is not running.

The first post-power-correction tracking attempt
`test-output/pipeline/20260908-182808-tracking/` failed pre-start control access
on node 0 and produced no centroid observations. During this interval, node 3
automatically recovered a real control-service failure: health changed from
recovery_count=0 to 1, last_fault=6, safe_boot=true, with reset uptime. This
demonstrates that the new service supervision can recover a naturally occurring
failure, but node 0 remained unreachable and the four-node trial did not pass.
All observed health identities were build `31359c922f9f1b88`.

### Post-power-correction tests and socket-budget finding (18:41–18:47)

Studio now accepts optional `target_node_ids` on tracking start, allowing a
selected subset when another node is unreachable. The 1/2/3 trial verified
selection, stop-to-OFF, and MCAP finalization. Duplicate and unknown IDs both
returned HTTP 400 (`test-output/pipeline/target-api-validation.json`); all 37
server tests passed. Empty/default selection retains fleet behavior.

`20260908-184136-tracking`: 60-second observation, 10,399 total MCAP packets
including startup/stop tails; nodes 1/2/3 delivered 48.07/46.92/49.77 FPS,
mean detection 1.903/1.973/1.911 ms, zero sequence gaps, zero capture failures,
zero detector overflow, zero reported TX drops, unchanged recovery counts.
All returned OFF. The analyzer failed with ReadTimeout at 16 MS/s: no new
clock result exists for this run. Screenshots and the failed run are retained.

`20260908-184315-preview`: 30-second unsynchronized QVGA Y8 TCP preview
requested via Wi-Fi. Nodes 1/2/3 delivered 194/42/136 distinct frames,
6.806/1.408/4.861 observed FPS, with no frame-request errors before cleanup.
The statistics command then closed on node 1. Nodes 1/2/3 all subsequently
showed increased recovery counters and fault 6; later health evidence confirms
node 2's reboot even though its immediate final health request timed out.
This establishes automatic reboot behavior, not a stable preview PASS.
The analyzer again failed ReadTimeout. Node 0 later responded without a new
recovery count, so earlier unreachability must not be called a proven hang.

Code inspection found CONFIG_LWIP_MAX_SOCKETS=10. Seven persistent sockets
(LAN, telemetry, centroids, command listener, HTTP listener/control, discovery)
plus TCP preview listener/client and both loopback-probe endpoints require
11 even before external command/health clients. Thus the new probe can itself
encounter socket exhaustion during preview. Earlier fault-6 observations
must not be presented as proof of recovering a driver deadlock. Increasing
the socket budget to 20 is the next controlled firmware change; no camera
allocation or streaming-rate changes are bundled with it. No memory-corruption
trace or hardware power diagnosis is available without UART.

### Socket fix built; external test infrastructure interrupted

Build `662952cc2cfa3bf0` successfully compiled with 20 sockets and a compile-time
minimum of 16. Application SHA256 is
`a4beb7c66edc127bccf6ab86520defce995d33679cdbae7f6247724bc2b5e6bd`.
It is archived under `test-output/timing-firmware/662952cc2cfa3bf0/`.
The initial OTA attempt `1788868237873539100` failed health preflight on all
four nodes; **no upload began**. Nodes therefore still ran `31359c922f9f1b88`
at that point. The flash watcher is stopped; its canonical image pin was updated.

The host's Wi-Fi Direct interface lost 192.168.137.1 and reverted to link-local.
Windows then reported WiFiDeviceOff when restarting the test hotspot. The
WinRT radio API returned Allowed to enable Wi-Fi but state remained Off and
hotspot start still failed. Ethernet's individual profile reports InternetAccess
although GetInternetConnectionProfile returns null; the hotspot helper now
accepts the unique InternetAccess profile in that case. Saved credentials
were kept private. The user was asked to enable Wi-Fi; no uplink configuration
or radio hardware was replaced.

Separately, the analyzer failed even a 5-second 8 MS/s four-channel test and a
1-second 4 MS/s single-channel test, both with ReadTimeout. These are USB
transport diagnostics with camera OFF, not timing results. The user was asked
to reconnect only analyzer USB, preserving probes/grounds. API captures do not
appear in the main Logic UI; the retained Cua window screenshot shows the
connected idle screen, not a waveform. Both infrastructure failures prevent
claiming a completed post-fix timing qualification until retested.

Studio was rebuilt with the 662952cc2cfa3bf0 canonical image and all 37 server
tests passed; Python harness syntax checks passed. Studio was restarted hidden.
The on-device socket fix and post-fix timing measurement remain unverified,
pending restoration of Wi-Fi and analyzer USB. No UART is required to resume
OTA once nodes rejoin. No Rust rewrite or memory-corruption claim was made.


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
