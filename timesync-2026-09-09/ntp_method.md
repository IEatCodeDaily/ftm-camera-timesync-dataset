# Reproducible software NTP-style baseline

Prepared 9 September 2026. Implementation and build verification are complete; this note alone is not evidence of an on-device measurement.

## Estimator and timestamps

The optional volatile `ntp` timing source uses a private UDP protocol, not RFC NTP or SNTP. It does not discipline a UTC clock and has no NTP packet format, stratum, clock filter, frequency servo, or multi-server selection. Its purpose is a simple software four-timestamp baseline against the same node-2 Wi-Fi MAC clock used as the FTM reference.

Each exchange records client transmit t1 immediately before `send`, server receive t2 immediately after `recvfrom`, server transmit t3 immediately before `sendto`, and client receive t4 immediately after `recv`. All values are microseconds from `esp_wifi_internal_get_mac_clock_time()`, extended from 32 bits near the local `esp_timer_get_time()` epoch using `strobe_mac_extend`. This extension requires the MAC and timer epochs to remain within 2^31 microseconds (35.8 minutes), as in the existing local-MAC strobe path. It handles individual 32-bit MAC rollovers; it is not a general persistent epoch service.

The client computes:

```
offset_us = ((t2 - t1) + (t3 - t4)) / 2
rtt_us    = (t4 - t1) - (t3 - t2)
reference_time_us = local_mac_us + offset_us
```

The latest accepted sample replaces the previous offset; slope is fixed at one. There is no smoothing, minimum-delay selection, drift fit, or oscillator adjustment. Equal forward/reverse delay makes the offset unbiased for constant-rate clocks over one exchange. Delay asymmetry contributes half the forward-minus-reverse delay; software scheduling and network-stack latency are included in the measured paths. The server processing interval is subtracted in the RTT estimator.

## Transport and acceptance gates

UDP port 7791 carries a 32-byte native little-endian structure: magic `0x3150544e`, uint32 sequence, and three int64 timestamps t1/t2/t3. Requests set t2/t3 to zero. The server echoes sequence/t1 and supplies t2/t3. The connected client socket restricts replies to the configured endpoint; clients check length, magic, sequence, and echoed t1. This private protocol assumes identical ESP32 builds and a trusted experiment LAN.

Clients begin a poll cycle approximately every 1,000 ms, with 10-ms task-delay granularity; scheduler delays can lengthen that interval. Receive timeout is 250 ms. An estimate is rejected if timestamp order is invalid, RTT is negative, or RTT exceeds 250,000 us. A client output clock is ready after one accepted sample and becomes unready when the last accepted update is at least 3,000,000 us old. No mandatory multi-sample warmup is implemented. For measurement, allow at least five poll cycles and retain status showing increasing `accepted`, recent `age_us`, and no unexpected rejection accumulation before recording edges. Server `accepted=0` and `age_us=-1` are normal: its reference offset is zero.

## Commands and shared output path

The preflight roster at `preflight/20260909-063130-669186/preflight.json` recorded node 2 at `192.168.137.177`; verify this address against the current roster after restart because it is a network assignment.

With each node's strobe mode OFF, configure:

```
# Node 2 only
timing ntp server
timing config mcpwm ntp

# Nodes 0, 1, and 3, each separately
timing ntp 192.168.137.177
timing config mcpwm ntp

# Each node: retain status before and after measurement
timing
```

Source selection starts the exchange service while the output mode is still OFF, so warmup can precede the existing 1HZ measurement command. Use the same 1HZ GPIO, MCPWM backend, sample rate, acquisition duration, channel mapping, and pulse-validity rules as the FTM and no-sync measurements. The strobe uses the same narrowest-span MAC/ESP timer pairing, next integer reference second calculation, and hardware MCPWM edge path. NTP only changes the clock mapping to unit slope plus software-estimated offset. The ordinary mode clock-status line still describes the FTM clock check; NTP readiness must be assessed using `timing` and physical pulses.

Leaving 1HZ does not stop the exchange service. With mode OFF, `timing config mcpwm ftm` stops it; select another non-NTP source before changing the endpoint. Configuration is RAM-only and reboot restores the default FTM source. The new task runs on control core 0 at priority 4 with a 4,096-byte stack.

Selecting NTP or local-MAC no-sync does not pause the existing FTM-ranging or LAN-coordination services. Background FTM traffic therefore remains unless the experiment controller explicitly disables it. A comparison with that traffic retained measures alternative output clock mappings under the existing firmware workload, not isolated protocol power, airtime, or minimum processing overhead. Record any explicit background-traffic changes with each trial.

## Source, verification, and lifecycle review

Canonical implementation directory: `E:/Projects/wireless-ir-mocap/firmware/ftm_clocksync/main/`. New files are `simple_ntp.c` and `simple_ntp.h`; `strobe_gpio.c` and `CMakeLists.txt` contain minimal source-selection/build additions. `strobe_clock.h` supplies epoch arithmetic; `task_cores.h` supplies placement. The archive retains these exact files under `firmware/3939b066398cec36/main/`, and the host tests under its `tools/` directory. Existing FTM estimator logic was not changed.

Lifecycle inspection found no evident normal-path ownership conflict: configuration is rejected while the service exists, source changes require the strobe OFF, disabling signals the loop and waits for task teardown, and the task closes its socket before releasing its handle. Binding/connection failure terminates the task and is visible as `running=0`; a client loses readiness after stale updates. Remaining unverified failure paths include socket-option failure (the timeout-setting return is not checked), abrupt network loss, and repeated task start/stop on hardware. Require live warmup/status confirmation before accepting a trial. The software review is not a substitute for that check.

Host C tests were compiled using `C:/Program Files/LLVM/bin/clang.exe` and executed successfully. `test_simple_ntp.c` checks a known offset, asymmetric-delay bias, independent epochs across a local MAC rollover, invalid ordering, negative RTT, and excessive RTT. `test_strobe_clock.c` checks existing MAC/FTM extension and independent-clock RTT arithmetic. Reproduction from the archived firmware directory:

```
clang tools/test_simple_ntp.c -o test_simple_ntp.exe
./test_simple_ntp.exe
clang tools/test_strobe_clock.c -o test_strobe_clock.exe
./test_strobe_clock.exe
```

The canonical build command was `cmd.exe /c tools\build_flash.bat build`, run in `E:/Projects/wireless-ir-mocap/firmware/ftm_clocksync`. It completed successfully with ESP-IDF 5.5.5 and produced `build-perf/ftm_clocksync.bin`; image validation passed. Binary size: 1,070,704 bytes; app partition: 0x400000 bytes (74% free). No device was flashed by the implementation subtask.

- Actual ELF/runtime build identity: `507f8d74b8d34447` (full ELF SHA256 `507f8d74b8d34447070eb8e3cb3adc42ff55c9ffcbdf6a9f936fcac2bfae5773`, verified using `esptool image_info --version 2`).
- Archive label `3939b066398cec36` is the image-validation hash prefix, **not** the runtime build ID. Full image validation hash: `3939b066398cec36683c4f4eeee87b2fbf4dcb1d79de58419d708c6134a68590`.
- Binary SHA256: `80a9b87dfc7aee92c5f3e9ccb707feaaf9616df7a31d161c8ce31ca010f592a1`.
- Archive: `firmware/3939b066398cec36/` contains binary, ELF, selected source files, tests, `host-tests.txt`, `ntp-baseline-build.log`, and `sha256.json`.
- Original build log: `E:/Temp/ntp-baseline-build.log`.

No sdkconfig file was copied into the report archive. This is a selected-source archive with the exact executable, not a complete standalone firmware source/dependency distribution.
