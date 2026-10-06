# Time synchronization in the wireless IR motion-capture platform

Experimental report | 8-9 September 2026 | Four ESP32-S3 nodes

Report revision: 2026-09-09 07:33 Asia/Jakarta. Numerical results recomputed from retained logic-analyzer transition CSVs. GPIO phase is the measured outcome; camera exposure and absolute UTC accuracy are outside the demonstrated evidence.

## Abstract

This report evaluates time synchronization in a four-node ESP32-S3 motion-capture prototype using externally recorded GPIO outputs. The existing campaign investigated fine timing measurement (FTM), access-point timing synchronization function (TSF), frozen FTM holdover and pulse-generation backends. Three clean historical 60-second records following an FTM timestamp-epoch repair contained 60 complete pulses per node, with follower-to-reference offset standard deviations of 0.481-0.704 microseconds and a largest observed absolute offset of 1.937 microseconds under network health polling. Additional source trials initially suffered unexpected transitions, missing qualified events and concurrent firmware interference. After a user reset, constant-level controls no longer exhibited late transitions. A common-firmware campaign then compared no synchronization, simple NTP-style software time transfer, live FTM, TSF and frozen FTM using the same MCPWM backend. In the completed common-firmware trials, FTM's largest observed relative phase is 1.688 microseconds; the simple NTP-style baseline's is 55.503 milliseconds. These are finite-record extrema across the completed trials, not guaranteed bounds. Nine of fifteen planned comparison recordings completed before repeated analyzer timeouts stopped acquisition. The measurements support an implementation-specific comparison while retaining failures, sample counts and apparatus changes. Exact board revision, long-uptime readiness, absolute clock accuracy and optical exposure alignment remain unestablished; the report makes no exposure-synchronization or universal protocol-performance claim.

Keywords: clock synchronization; ESP32-S3; FTM; NTP-style time transfer; TSF; logic analyzer.

## 1. Research question and scope

The practical question is whether the platform can align timestamp-derived events across camera nodes tightly enough to support future motion-capture fusion, and how the implemented synchronization choices compare. Three subquestions guide the analysis: (1) what relative phase, bias and jitter are observed for each clock source; (2) how much does GPIO generation contribute; and (3) which conditions prevent a valid comparison?

The experimental unit is a timed hardware recording of a fixed four-node system, not an individual pulse. Nodes 0, 1 and 3 are followers and node 2 is the reference. There are only three follower devices, and all pairwise series share the same reference edge. Sequential pulses and trials on the same devices are not independent draws from a device population. Results are descriptive, without per-pulse significance tests or confidence intervals that imply independent hardware replication.

The report applies modular evidence tracing, report compilation, statistical visualization and adversarial review guidance from Imbad0202's academic-research-skills repository, pinned at commit `8e4c8777648cdb3a1b8c01e956cf902121216aa0`. This is an experimental report, not a systematic literature review, a completed journal-submission pipeline, or an independent peer-review certification. AI assisted the source audit, code, analysis and drafting; raw measurements remain the evidential basis.

## 2. Methods and definitions

### 2.1 Apparatus and acquisition

Four ESP32-S3 camera boards provide GPIO41 timing outputs to analyzer D0-D3; GPIO2 is reserved for identification. Node 2 supplies the reference pulse. The retained apparatus record identifies a software-reported Logic device, device ID `8BBBCD9E91E0E1D6`, and Logic 2.4.46. The supplied setup photo shows an analyzer marked 24 MHz / 8CH; physical manufacturer authenticity is unverified. Successful comparison recordings use 16 MS/s, corresponding to a 62.5-ns sample interval. The additional constant-level controls also use 8 MS/s. Sample interval is resolution, not calibrated measurement accuracy. Channel skew, cable delay and analyzer timebase error were not calibrated. The user confirmed on 9 September that probes and common-ground wiring were unchanged and connected. [E1, E2, E5]

The nominal diagnostic waveform is one pulse per second, approximately 50 ms HIGH. The camera is OFF in the clock-source comparison. Original repaired FTM records applied up to five health requests per second per node; actual successful counts were 233-239 per record. New source trials apply no deliberate health polling during acquisition, but ordinary background network/FTM services remain active. NTP-style selection adds its exchange traffic. No protocol airtime, energy, range, interference distribution or camera-exposure measurements are reported.

### 2.2 Operational clock-source comparison

| Method | Mapping used by the output | Interpretation |
| --- | --- | --- |
| No timesync / mac | Local MAC clock, slope 1, offset 0 | Unsynchronized output epochs; background FTM still runs |
| Simple NTP-style / ntp | Latest software four-timestamp offset, slope 1 | Private UDP estimator; not an RFC NTP/SNTP implementation |
| Live FTM / ftm | Continuously updated affine local-to-node-2 MAC fit | Hardware Wi-Fi timestamps with software fitting and mapping |
| Frozen FTM / frozen | Initial valid affine slope and offset retained | Holdover after synchronization, not a no-sync control |
| AP TSF / tsf | Each station reads its access-point TSF | Common hotspot clock; a different reference domain from node-2 MAC |

For every source, the new comparison selects the same MCPWM hardware pulse path. TSF therefore compares physical node-to-node output phase, but it is not a direct estimate of offset to the node-2 MAC clock. The original same-build source comparison instead uses GPTimer for all three sources. Backends and sources must not be conflated: a timer callback change is not a different synchronization protocol. [E2, E3, E6]

FTM supplies four wireless event timestamps for round-trip measurement; the platform uses its resulting pairs for an affine clock fit. Espressif documents the FTM initiator/responder procedure and timestamp-based RTT calculation. FTM's timestamp unit does not itself establish achievable clock accuracy. (Espressif Systems, n.d.)

### 2.3 Simple NTP-style baseline

The added baseline uses node 2 as a local UDP server, with software timestamps immediately before transmission and immediately after reception. For client transmit/receive times t1/t4 and server receive/transmit times t2/t3, it computes offset = [(t2 - t1) + (t3 - t4)] / 2 and RTT = (t4 - t1) - (t3 - t2). These are the four-timestamp estimators described in RFC 5905. Unequal forward and reverse delays bias the offset by half their difference. A low measured RTT does not prove equal path delays. (Mills et al., 2010, Section 8.)

This experiment uses a private 32-byte UDP structure on port 7791, extended MAC microseconds, approximately one poll per second, the latest accepted offset, a 0-250 ms RTT gate and a three-second freshness gate. It has no minimum-delay filter, frequency servo, server selection or UTC discipline. The source preserves the default FTM estimator and uses the same MAC-to-timer pairing and MCPWM path. It is deliberately labeled NTP-style; its results must not be generalized to an optimized NTP daemon, RFC-compliant SNTP, PTP or hardware-timestamped network time transfer. The implementation note contains commands, timestamp locations and tests. [E6]

### 2.4 Reanalysis and acceptance rules

The reanalysis reads every transition in `digital.csv`. An initial HIGH level is not an observed rising edge. A complete HIGH interval qualifies only when its width is 40-65 ms. There is no startup deletion, digital glitch filter, outlier removal, bias subtraction or detrending in the main statistics. Every rejected complete pulse remains counted. Qualified results with extra transitions are conditional subset diagnostics, not signal-clean performance.

For each follower, qualified rising edges are matched to reference edges within +/-0.5 seconds, one-to-one, by increasing absolute separation; matched pairs are then ordered in time. This measures nearest periodic phase. It cannot detect integer-second epoch errors. The broader half-second window retains the no-sync baseline; the historical diagnostic used a quarter-second window, while the original raw nearest-edge summary permitted reference reuse. Their estimands and counts are not interchangeable. The reanalysis has explicit uniqueness and distance-gate checks.

For phase differences d in microseconds, the report records mean(d), population SD, RMS, nearest-sample p95 of abs(d), max(abs(d)), counts and an optional descriptive linear trend. The code uses NumPy's nearest-sample quantile rule; boundary rounding can differ from other quantile conventions. A small SD with a large mean is a stable phase error, not accurate alignment. Trend is not an independently calibrated oscillator-drift estimate.

At least 30 matched pulses is only a descriptive-data threshold. Signal quality, cadence performance and synchronization error are assessed separately. A clean signal contains no rejected complete HIGH pulses. A 0.99-1.01-second interval gate flags cadence variation; intervals above 1.5 seconds additionally flag likely missed cycles. A method can produce a valid clean measurement yet fail the cadence or offset criterion. Changed firmware, disconnected probes and acquisition failures invalidate comparisons. Width filtering cannot retrospectively validate retained timestamps from corrupted signals. Complete-pulse counts can differ by one at capture boundaries; all unmatched counts and cadence intervals are exported. [E7]

## 3. Results from the 8 September campaign

### 3.1 Epoch-repaired FTM with MCPWM

The three clearest repaired records comprise 180 recorded seconds per node split across two firmware builds and a restart. They are not one continuous 180-second trial. Each contains 60 matched pulses for all three followers, with no rejected HIGH pulses. Table 1 and Figure 1 retain the finite-run offsets. [E2]

Table 1. Recomputed repaired FTM/MCPWM results (microseconds).

| Run | Follower | n | Mean | SD | p95 abs | Max abs |
| --- | --- | --- | --- | --- | --- | --- |
| 20260908-173016/00-mcpwm-ftm | 0 | 60 | -0.079 | 0.704 | 1.437 | 1.562 |
| 20260908-173016/00-mcpwm-ftm | 1 | 60 | -0.310 | 0.611 | 1.187 | 1.562 |
| 20260908-173016/00-mcpwm-ftm | 3 | 60 | -0.462 | 0.522 | 1.125 | 1.188 |
| 20260908-173016/01-mcpwm-ftm | 0 | 60 | -0.132 | 0.608 | 1.250 | 1.500 |
| 20260908-173016/01-mcpwm-ftm | 1 | 60 | -0.234 | 0.635 | 1.188 | 1.625 |
| 20260908-173016/01-mcpwm-ftm | 3 | 60 | -0.523 | 0.481 | 1.124 | 1.500 |
| 20260908-174159/00-mcpwm-ftm | 0 | 60 | 0.063 | 0.578 | 0.938 | 1.937 |
| 20260908-174159/00-mcpwm-ftm | 1 | 60 | -0.392 | 0.582 | 1.312 | 1.812 |
| 20260908-174159/00-mcpwm-ftm | 3 | 60 | -0.216 | 0.563 | 1.313 | 1.438 |

![Figure 1](analysis/historical_ftm.png)

Figure 1. All recomputed qualified pulse offsets from the three clean repaired records, follower minus node 2. No outliers are removed. Trials 00/01 use build `33a74b5c18b078e9`; the final restart trial uses `e0ac899e423e2d93`. Camera OFF, health-polling load, 16 MS/s.

The largest observed offset of 1.937 microseconds is below the project's 1-ms relative-output objective for these records. It is not a guaranteed 1.937-microsecond bound and does not establish a sub-microsecond maximum error: sub-microsecond SD and maximum error are different claims. The later installed firmware differs, so historical validation must remain linked to its tested builds.

### 3.2 Exploratory same-backend source comparison

Table 2. Original 25-second GPTimer source trials on `34e1a3df22c26af8` (SD in microseconds; 25 pulses per node).

| Source | SD node 0 | SD node 1 | SD node 3 | Worst abs |
| --- | --- | --- | --- | --- |
| Live FTM | 0.452 | 0.581 | 0.388 | 1.500 |
| AP TSF | 5.851 | 4.958 | 6.478 | 35.125 |
| Frozen FTM | 1.132 | 13.675 | 41.390 | 211.501 |

These single sequential trials favor live FTM in their recorded output metrics, but their firmware predates the epoch repair. The data do not establish a general ranking or isolate uncertainty caused by clock aging, run order or path conditions. They include no no-sync or NTP baseline. Frozen FTM remains a holdover experiment. Table 2 quotes the retained original summaries and is distinguished from the new reanalysis. [E3]

### 3.3 Output backend and failure history

Table 3. Representative original same-build FTM backend trials, 25 seconds each, build `34e1a3df22c26af8`.

| Backend | SD range across followers (us) | Largest abs offset (us) | Qualification |
| --- | --- | --- | --- |
| Task | 0.530-1.581 | 5.375 | Single short trial |
| esp_timer callback | 22.136-28.741 | 110.062 | Single short trial |
| GPTimer | 0.388-0.581 | 1.500 | Single short trial |
| High-priority task | Not measured | Not measured | Analyzer ReadTimeout |

Changing edge generation substantially changed output jitter in these trials, showing why source comparisons need a common backend. Later high-priority-task, paired-GPTimer and MCPWM trials also exist in the inventory. They are not pooled across firmware and uptime changes. [E3]

The early implementation applied a MAC-clock fit to esp_timer-domain values; a clean GPIO41 recording exposed millisecond biases. Subsequent 48-bit FTM timestamp wrapping produced approximately 50/425/475-ms phase jumps. The repair extended timestamp epochs using coarse reference announcements and reset the fit on reference identity changes. The log's pre-repair 60-second trials retain the failures. A four-second record with only node 2 armed is missing a comparison, despite an old inventory displaying worst=0.000. Later D3-disconnected and reconnection records remain inconclusive. Analyzer timeouts and export failures contribute no valid zero-error result. [E1, E2, E3]

## 4. Additional measurements on 9 September

### 4.1 Readiness and acquisition checks

Preflight on `fdb0d3b4ab514a2d` found all three followers model-valid but `clock_ready=0` after approximately eight hours of operation; node 2 remained ready. Commands to restart produced timeout responses, while subsequent health uptimes confirmed reboot and later arming telemetry showed readiness restored. The timeouts are retained. Failure onset and root cause were not established, and recovery by restarting is not a long-uptime fix. [E4]

A five-second post-restart FTM check produced five intact pulses per channel but thousands of additional transitions. The source comparison then exercised 60-second MAC, TSF, live FTM and frozen FTM captures at 16 MS/s. Source order was fixed, not randomized, for this initial diagnostic round. Firmware and health were checked before/after. The firmware identity check detected a concurrent node-1 update during frozen FTM; that run is invalid. [E5]

### 4.2 Controlled same-firmware comparison after reset

After the user reset all nodes, the two-second 16 MS/s constant-LOW control had zero transitions on all channels. Constant HIGH had only the known initial transition at 128 microseconds. At 8 MS/s each channel had only one initial transition at 256 microseconds, with no later transitions. A subsequent five-second NTP-style check had five clean complete pulses per node; FTM had five complete pulses plus a boundary-incomplete rise, with no rejected pulses. [E5]

The other hardware-control task then paused all uploads, builds, mode changes, server restarts and analyzer actions. Nodes 0/2/3 were installed from the immutable archived image; node 1 already ran that same image. All four runtime identities were checked as `507f8d74b8d34447`. The shared workspace snapshot also contains unrelated camera diagnostic additions; the experiment holds the complete image fixed across sources and keeps cameras OFF. No causal comparison is made across different images. Firmware upload/readback evidence is `test-output/ota-fleet/1788911291048392600/` and the prior node-1 record `1788910716585756100/`.

The matrix targets three 60-second runs per source. Orders rotate across blocks: FTM, NTP-style, MAC, TSF, frozen; NTP-style, MAC, TSF, frozen, FTM; MAC, TSF, frozen, FTM, NTP-style. This is partial order counterbalancing, not randomization or a complete Latin-square design. It keeps a single boot/configuration generation, with source rearming between captures. NTP-style gets 12 seconds of warmup after selection; other sources get three seconds. Ordinary FTM background traffic continues for all sources. Each NTP trial retains accepted-update counts and age before/after acquisition.

Nine 60-second comparison recordings completed: the first block and the first four sources of block two. The second block's FTM attempt ended with an analyzer ReadTimeout and no exported CSV. A five-second FTM check then succeeded without a further board reset, but additional FTM attempts at 60 seconds/16 MS/s, 30 seconds/16 MS/s and 60 seconds/8 MS/s also timed out. Acquisition was stopped after those repeated failures. There is consequently one new 60-second FTM trial and two each for the other sources; the three-block target was not completed. Historical FTM repetitions provide separate evidence and are not substituted into the common-firmware matrix. Only successful, signal-clean, same-build recordings with at least 30 matched pulses per follower enter Table 4. Clean cadence failures remain eligible; the missing-data exclusions do not depend on error magnitude.

Table 4. Common-firmware source results from completed runs. Ranges span per-node, per-run estimates, not pooled independent samples. Largest p95 is the maximum of the individual pair-series p95 values. Clean signal means no rejected complete HIGH pulses; cadence failures remain separately reported.

| Source | Runs | N per pair | SD range (us) | Largest p95 (us) | Largest abs (us) | Clean signal |
| --- | --- | --- | --- | --- | --- | --- |
| No timesync | 2 | 59-60 | 6.589-105.094 | 186,878.188 | 186,896.250 | 2/2 |
| Simple NTP-style | 2 | 59-60 | 2,322.845-8,163.658 | 15,239.063 | 55,503.062 | 2/2 |
| Live FTM | 1 | 60-60 | 0.607-0.749 | 1.563 | 1.688 | 1/1 |
| AP TSF | 2 | 59-60 | 13.835-28.740 | 60.187 | 182.188 | 2/2 |
| Frozen FTM | 2 | 59-60 | 0.539-0.800 | 1.501 | 1.937 | 2/2 |

In the completed common-firmware trials, FTM's largest observed relative phase is 1.688 microseconds; the simple NTP-style baseline's is 55.503 milliseconds. These are finite-record extrema across the completed trials, not guaranteed bounds.

Table 4b. Observed inter-pulse intervals longer than 1.5 seconds, summed across completed runs. These indicate missing/late cycle delivery within a recording, not merely incomplete boundary pulses. They are output reliability failures even when the surviving phase offsets are small. Root cause is not isolated by this comparison.

| Source | Node 0 | Node 1 | Reference 2 | Node 3 |
| --- | --- | --- | --- | --- |
| No timesync | 0 | 1 | 0 | 1 |
| Simple NTP-style | 1 | 0 | 0 | 0 |
| Live FTM | 0 | 0 | 0 | 0 |
| AP TSF | 0 | 1 | 0 | 1 |
| Frozen FTM | 1 | 1 | 0 | 1 |

The finite absolute-phase statistics describe delivered pulses. A small value does not imply uninterrupted delivery. In the first controlled block, the raw clean records contain approximately two-second gaps in NTP-style node 0, local-MAC node 3, TSF node 1 and frozen-FTM nodes 0/3. Because missing cycles occur across several sources sharing the output backend, they cannot be assigned uniquely to a synchronization estimator. NTP-style also shows shorter period variations consistent with its changing estimated offset. Exported period extrema and gap counts preserve both behaviors. In each of the first two NTP trials, every client gained 62 accepted updates with no new rejection, and all observed endpoint ages were below 0.815 seconds. This supports an active estimator at the sampled endpoints; it cannot rule out an unobserved transient between them. Exact status and build/uptime continuity checks are retained in `ntp_hardware_audit.md`.

### 4.3 All additional source runs, including failed pilots

Table 5. All new source runs, including short checks and failed acquisitions. Durations are shown in each run label. SD ranges describe surviving follower subsets. Max is the largest qualified absolute phase among followers; it is not an all-edge error bound. N lists matched node 0/1/3 pulses. All runs use 16 MS/s except the failed `070851-campaign` attempt at 8 MS/s. The early corrupted pilot and firmware-contaminated run are retained.

| Run | Build | N 0/1/3 | SD range (us) | Max abs (us) | Status |
| --- | --- | --- | --- | --- | --- |
| 20260909-063251-campaign/00-00-mcpwm-ftm (5 s) | fdb0d3b4ab514a2d | 5/5/5 | 0.220 to 0.331 | 0.875 | Short check: glitches |
| 20260909-063455-campaign/00-00-mcpwm-mac (60 s) | fdb0d3b4ab514a2d | 60/60/50 | 6.903 to 97.744 | 449,514.750 | provisional glitches |
| 20260909-063455-campaign/00-01-mcpwm-tsf (60 s) | fdb0d3b4ab514a2d | 54/56/59 | 14.976 to 18.230 | 97.313 | provisional glitches |
| 20260909-063455-campaign/00-02-mcpwm-ftm (60 s) | fdb0d3b4ab514a2d | 47/52/42 | 0.421 to 0.582 | 1.688 | provisional glitches |
| 20260909-063455-campaign/00-03-mcpwm-frozen (60 s) | fdb0d3b4ab514a2d | 46/6/43 | 0.364 to 1,087.250 | 7,214.063 | INVALID: mixed firmware |
| 20260909-064935-campaign/00-00-mcpwm-ntp (5 s) | 507f8d74b8d34447 | 5/5/5 | 850.622 to 3,479.901 | 5,698.063 | Short check: clean |
| 20260909-064935-campaign/00-01-mcpwm-ftm (5 s) | 507f8d74b8d34447 | 5/5/5 | 0.567 to 0.808 | 1.188 | Short check: clean |
| 20260909-065110-campaign/00-00-mcpwm-ftm (60 s) | 507f8d74b8d34447 | 60/60/60 | 0.607 to 0.749 | 1.688 | clean descriptive |
| 20260909-065110-campaign/00-01-mcpwm-ntp (60 s) | 507f8d74b8d34447 | 59/60/60 | 2,322.845 to 7,602.697 | 39,774.188 | Clean; cadence failed |
| 20260909-065110-campaign/00-02-mcpwm-mac (60 s) | 507f8d74b8d34447 | 60/60/59 | 6.776 to 104.779 | 185,233.812 | Clean; cadence failed |
| 20260909-065110-campaign/00-03-mcpwm-tsf (60 s) | 507f8d74b8d34447 | 60/59/60 | 18.620 to 28.740 | 176.188 | Clean; cadence failed |
| 20260909-065110-campaign/00-04-mcpwm-frozen (60 s) | 507f8d74b8d34447 | 59/60/59 | 0.539 to 0.670 | 1.624 | Clean; cadence failed |
| 20260909-065110-campaign/01-00-mcpwm-ntp (60 s) | 507f8d74b8d34447 | 60/60/60 | 3,213.145 to 8,163.658 | 55,503.062 | Clean; cadence failed |
| 20260909-065110-campaign/01-01-mcpwm-mac (60 s) | 507f8d74b8d34447 | 60/59/60 | 6.589 to 105.094 | 186,896.250 | Clean; cadence failed |
| 20260909-065110-campaign/01-02-mcpwm-tsf (60 s) | 507f8d74b8d34447 | 60/60/59 | 13.835 to 27.294 | 182.188 | Clean; cadence failed |
| 20260909-065110-campaign/01-03-mcpwm-frozen (60 s) | 507f8d74b8d34447 | 60/59/60 | 0.733 to 0.800 | 1.937 | Clean; cadence failed |
| 20260909-065110-campaign/01-04-mcpwm-ftm (60 s) | 507f8d74b8d34447 | 0/0/0 | NA | NA | No CSV: acquisition failed |
| 20260909-070546-campaign/00-00-mcpwm-ftm (5 s) | 507f8d74b8d34447 | 5/5/5 | 0.505 to 0.741 | 1.250 | Short check: clean |
| 20260909-070610-campaign/00-00-mcpwm-ftm (60 s) | 507f8d74b8d34447 | 0/0/0 | NA | NA | No CSV: acquisition failed |
| 20260909-070748-campaign/00-00-mcpwm-ftm (30 s) | 507f8d74b8d34447 | 0/0/0 | NA | NA | No CSV: acquisition failed |
| 20260909-070851-campaign/00-00-mcpwm-ftm (60 s) | 507f8d74b8d34447 | 0/0/0 | NA | NA | No CSV: acquisition failed |

The NTP-style physical trials retain live exchange status, exact firmware identity and external pulses.

The no-sync baseline deliberately retains its initial nearest-second phase. Different boot epochs create an arbitrary phase component of hundreds of milliseconds; that magnitude is not a reproducible oscillator property. Its phase trend and jitter are distinct from its initial bias. An improvement factor using that arbitrary baseline would be misleading.

The early corrupted FTM and TSF pilot estimates are conditional on retaining complete pulse widths. Missing qualified events and a corrupted reference can select which offsets survive. Their small conditional SDs cannot establish fleet-wide timing quality. The invalid frozen pilot includes an approximately 7.2-ms qualified excursion, while some p95 values remain near a few microseconds; a percentile alone can conceal sparse large events. Because that trial experienced a concurrent firmware update, neither its excursions nor missing events are attributed to frozen FTM itself. It is excluded from the controlled comparison.

![Figure 2](analysis/new_comparison.png)

Figure 2. Controlled common-firmware trials after reset, separated by source and repeat. Axes are logarithmic. The title reports whether all included signals were clean; corrupted pilots and the mixed-firmware trial are excluded from this figure and retained in Table 5. A clean signal with large offsets/cadence variation remains evidence of method performance, not an acquisition failure.

### 4.4 Constant-level controls and GPIO41 review

Two-second controls held GPIO41 LOW and HIGH at both 16 and 8 MS/s. At 16 MS/s, commanded LOW yielded transition counts 24/0/2554/2714 across D0-D3; commanded HIGH yielded 1/1/1/64305. The single initial HIGH transitions occurred at 128 microseconds, consistent with the earlier documented acquisition-start artifact. D3 continued transitioning later. At 8 MS/s, LOW counts were 1/1/1547/54495 and HIGH counts were 1/1/1/12911; isolated initial transitions moved to 256 microseconds. Thus lowering the sample rate did not remove the issue. [E5]

The pre-reset controls establish a signal/acquisition problem under constant commands; they do not identify whether the cause is probing, grounding, electrical behavior, analyzer input, concurrent mode control or software decoding. The user confirmed connected wiring, inspected the running Logic session and reported resetting all boards without electrical changes. The post-reset controls and separate campaign are therefore a new apparatus-state block. The intervention strengthens a state-dependent explanation, but neither identifies a specific software defect nor proves a permanent fix. Pre-reset 16 MS/s LOW excursions were one sample wide and commonly simultaneous across channels, unlike ordinary 10-us frame-grab pulses. Final two-second static controls at `20260909-070956-static`, after the longer-capture timeouts, again had no late transitions: LOW at 16 MS/s had zero transitions; the other controls had only the sample-2048 startup transition. The timeouts and earlier spikes are distinct observed symptoms with no demonstrated common cause. The separate `noise_forensics.md` records the evidence and competing explanations.

The board manufacturer/revision is unknown; the user describes an ESP32-S3 CAM with rear microSD. Source inspection found GPIO41 assigned to timing/frame-grab output, no camera bus pin overlap and no application SD initialization. In 1-Hz mode the frame-grab function suppresses its own pulses. Freenove's official candidate-board pinout assigns SD CMD/CLK/DATA to 38/39/40 and exposes GPIO41 as GPIO/MTDI; this does not identify the user's clone. Other camera boards differ. No specific microSD conflict or pin-ownership bug is established. Full findings and primary pinout links are in `gpio41_audit.md`.

## 5. Camera load, interpretation and remaining limits

The retained camera-loaded `20260908-175800-tracking` capture differs from the health-polling trials. It contains 60 width-qualified clock pulses per channel during detection near 49-50 FPS. Its qualified follower means are 0.230/0.875/0.325 microseconds, SDs 0.900/0.881/0.723 microseconds and worst offsets 2.063/2.750/1.562 microseconds. However, D2 and D3 also contain 97 and 25 single-sample HIGH glitches. This is provisional loaded-clock evidence, not a clean all-edge pass. Later throughput-only successes do not supply missing timing measurements. [E8]

The measurement chain combines the FTM/TSF/software clock mapping, MAC-to-timer conversion, timer scheduling, peripheral output, reference-edge jitter and acquisition system. Its components cannot be separated from these traces alone. FTM-fit residuals of a few nanoseconds are internal consistency measures, not independently measured GPIO or exposure accuracy. Likewise, the analyzer's approximately 0.999968-second periods cannot establish absolute oscillator error without timebase calibration. GPIO strobes and frame-grab software timestamps do not measure exposure start or midpoint, including rolling-shutter row timing. [E1, E8]

The common-firmware measurements support a comparison of these implementations on this apparatus. They do not establish a universal no-sync/NTP/FTM ranking. Historical repaired FTM recordings provide additional bounded observations. The initial signal corruption and firmware interference remain failures; later clean results do not erase them. Limited independent repetitions, unknown environmental conditions and unresolved long-uptime readiness constrain stronger claims. PTP and other unimplemented methods are unmeasured; no values are borrowed from unrelated devices or simulations.

## 6. Conclusions and next qualification

FTM plus MCPWM achieved sub-microsecond offset SD and below-2-microsecond worst observed relative GPIO offset in three clean historical 60-second records. In the completed common-firmware trials, FTM's largest observed relative phase is 1.688 microseconds; the simple NTP-style baseline's is 55.503 milliseconds. These are finite-record extrema across the completed trials, not guaranteed bounds. Earlier experiments show that correct clock domains, epoch extension and edge generation are essential: a low-jitter backend cannot repair a wrong clock epoch. The unsynchronized control demonstrates large phase despite reasonably regular pulses. The simple latest-sample NTP-style baseline includes software and Wi-Fi scheduling uncertainty and should not represent every NTP implementation.

Further qualification should first restore reliable sustained acquisition and complete the missing repetitions. Then extend a separate continuous run beyond several 281.475-second FTM wraps and repeat the source matrix under actual camera load, controlled interference and multiple apparatus sessions. Report warmup/update age, missing pulses, all observed extremes and recovery after reference restart. The incomplete three-block target is an initial repeatability design, not a power calculation or guarantee. A long-uptime test should address the observed eight-hour readiness loss, and physical board identification should settle GPIO41 routing.

For a full NTP claim, implement and qualify the intended RFC-compatible client configuration, or retain the narrower NTP-style label. For motion-capture claims, separately measure optical exposure alignment against a common visible event and evaluate reconstruction error using those timing records. No exposure or reconstruction result is inferred from this report's GPIO traces.

## 7. Reproducibility and evidence map

Run `analyze.py` with the project's `test-output/saleae-automation-env/Scripts/python.exe`, then `build_report.py` to regenerate tables, figures, Markdown and PDF. The report folder contains acquisition scripts, per-run/per-node CSV, matched offsets, machine-readable run classifications and SHA256 manifests. A retained copy of historical raw data and project notes is under `historical-evidence/`, preserving source-relative paths; analysis automatically prefers this copy. New raw CSV/Saleae files are under `new-acquisition/`. The selected NTP firmware archive includes source additions, exact binary/ELF, build log and passing arithmetic tests, but is not a complete standalone ESP-IDF distribution. [E6, E7]

| Evidence | Location relative to stated root | Use |
| --- | --- | --- |
| E1 - source repository | docs/TIMESYNC_MEASUREMENT_2026-09-08.md | Apparatus, clock-domain history, calibration limits |
| E2 - source repository | test-output/timing-methods/20260908-173016 and 20260908-174159 | Clean repaired FTM raw CSV and result JSON |
| E3 - source repository | docs/TIMESYNC_METHOD_RESULTS_2026-09-08.md; test-output/timing-methods/ | Full original inventory and backend/source trials |
| E4 - report folder | preflight/20260909-063130-669186/; new-acquisition/20260909-063251-campaign/ | Long-uptime failure and post-restart short check |
| E5 - report folder | new-acquisition/; noise_forensics.md; gpio41_audit.md; session_log.json; hardware-release.json | Pilot, nine controlled successes, failed retries, short checks and static controls through 20260909-070956 |
| E6 - report folder | ntp_method.md; ntp_hardware_audit.md; firmware/3939b066398cec36/ | Exact NTP-style method, tests, live update audit and executable |
| E7 - report folder | analyze.py; analysis/run_metrics.json; analysis/per_run_node.csv; analysis/manifest.json | Reanalysis, quality flags, numerical results and provenance |
| E8 - source repository | docs/PIPELINE_TIMESYNC_2026-09-08.md; test-output/pipeline/20260908-175800-tracking/ | Camera-loaded qualified pulses and glitches |

Source repository root: `E:/Projects/wireless-ir-mocap`. Report root: `E:/OneDrive/College/Thesis v2/experiments/reports/timesync-2026-09-09`. The experiment timestamps are local Asia/Jakarta unless a JSON field explicitly states UTC. The source corpus contains changing build identities; each table preserves its acquisition generation.

## References

Espressif Systems. (n.d.). *Wi-Fi driver: ESP32-S3, ESP-IDF v5.5.1*. [Official documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-guides/wifi.html). Accessed 9 September 2026. FTM timestamp procedure also documented in the [v5.5.1 FTM example](https://github.com/espressif/esp-idf/blob/v5.5.1/examples/wifi/ftm/README.md).

Freenove. (n.d.). *ESP32-S3 WROOM pinout* [Diagram]. [Official board pinout](https://raw.githubusercontent.com/Freenove/Freenove_ESP32_S3_WROOM_Board/main/ESP32S3_Pinout.png). Accessed 9 September 2026. Conditional candidate-board comparison only; the user's manufacturer/revision is unverified.

Imbad0202. (2026). *Academic research skills* [Software, repository snapshot]. [Repository](https://github.com/imbad0202/academic-research-skills). Commit `8e4c8777648cdb3a1b8c01e956cf902121216aa0`. Modular guidance used for report structure, evidence audit, plotting and writing review; empirical data did not come from this repository.

Mills, D., Martin, J. (Ed.), Burbank, J., & Kasch, W. (2010). *Network Time Protocol version 4: Protocol and algorithms specification* (RFC 5905). Internet Engineering Task Force. [RFC 5905, Section 8](https://www.rfc-editor.org/rfc/rfc5905.html#section-8).

Wireless IR motion-capture project. (2026, September 8-9). *Timing-method records, firmware and acquisition evidence* [Unpublished experimental data]. Local sources E1-E8 above. These records supply all reported device measurements.
