# NTP-style hardware service validation

This read-only audit covers completed NTP records `00-01-mcpwm-ntp` and `01-00-mcpwm-ntp` in `new-acquisition/20260909-065110-campaign/`. Both `result.json` files identify completed 60-second, 16-MS/s captures on firmware `507f8d74b8d34447`, with no deliberate health polling during capture. The planned third NTP repeat was not available when this subsection was written. This validates observed service operation; the comparative physical-edge analysis determines synchronization accuracy separately.

## Service readiness and updates

Both records' `timing-armed.json` and final `telemetry.json` show `backend=mcpwm source=ntp` and `running=1` on all four nodes. Node 2 is consistently the server; nodes 0, 1, and 3 are clients. Node 2 reports offset zero, accepted/rejected zero, and age -1, which is the expected reference-server status, not a failed client update.

The following are exact snapshot values. Each arrow means armed snapshot to final telemetry snapshot; ages and latest RTTs are in microseconds.

| Record | Node | Accepted | Rejected | Age (us) | Latest RTT (us) |
|---|---:|---:|---:|---:|---:|
| 00-01 | 0 | 12 → 74 | 1 → 1 | 814,330 → 592,599 | 4,809 → 11,133 |
| 00-01 | 1 | 12 → 74 | 1 → 1 | 800,135 → 414,887 | 14,216 → 8,521 |
| 00-01 | 3 | 13 → 75 | 0 → 0 | 713,579 → 523,732 | 3,837 → 4,776 |
| 01-00 | 0 | 12 → 74 | 1 → 1 | 785,788 → 681,243 | 4,833 → 40,510 |
| 01-00 | 1 | 12 → 74 | 1 → 1 | 792,651 → 387,568 | 3,660 → 11,429 |
| 01-00 | 3 | 13 → 75 | 0 → 0 | 799,542 → 481,840 | 3,580 → 4,583 |

Every client had 12 or 13 accepted exchanges before capture and gained exactly 62 accepted updates by the final telemetry snapshot, with no additional rejection. This is consistent with the configured 1-second poll interval over acquisition plus command/capture overhead; it does not establish an exact 62-second snapshot interval. The single rejection on nodes 0 and 1 was already present at arming. Its cause is not recorded, so it must not be assigned to capture load or startup ordering.

All observed client ages were below 0.815 seconds, comfortably inside the 3-second readiness gate. Observed latest RTTs were 3.580–40.510 ms, inside the 250-ms acceptance threshold. These are endpoint samples, not the RTT distribution across all exchanges. The final ring buffers retain only 32 pulse records per node. Full per-exchange t1/t2/t3/t4 and RTT histories were not logged.

Absolute offset values span different local boot epochs. For example, node 1's armed offset was -288,179,009 us in record 00-01; this is a coordinate transformation between clocks, not a 288-second synchronization error. Evaluate synchronization error from corresponding physical reference and peer edges.

## Build and health continuity

Campaign `health-before.json` and both NTP `health-after.json` files report the same build ID `507f8d74b8d34447` for all nodes, `recovery_count=0`, `last_fault=0`, and `safe_boot=false`. Uptime increases for each node:

| Node | Campaign before (us) | After 00-01 (us) | After 01-00 (us) |
|---|---:|---:|---:|
| 0 | 132,261,462 | 275,519,044 | 551,670,349 |
| 1 | 420,537,277 | 563,789,490 | 839,938,211 |
| 2 | 132,416,229 | 275,637,650 | 551,802,169 |
| 3 | 132,520,545 | 275,781,317 | 551,906,886 |

These snapshots provide no evidence of reboot, recovery escalation, safe boot, or a firmware change during the audited interval. They are discrete health checks, not a continuous fault trace. The campaign is the common-build exclusive measurement window; earlier captures affected by a concurrent firmware update are separate evidence and must not be pooled with it.

## Interpretation and limits

The physical analysis flags a roughly 2-second inter-pulse gap on node 0 in the first NTP record. Similar missing-cycle behavior also occurs with other timing sources using the shared output path. This service audit cannot assign the gap to the NTP estimator: endpoint counters show continuing successful exchanges, but neither counters nor the final 32-record pulse buffer reconstruct every instant of the capture. Report missing pulses separately from timing error and retain all valid paired edges using the prespecified matching rule.

This implementation is a private software NTP-style four-timestamp estimator with a latest-offset update and unit slope, not RFC NTP/SNTP. The physical MCPWM output path is shared with the FTM, local-MAC, TSF and frozen-fit conditions. Selecting `ntp` does not itself stop background FTM ranging or LAN coordination. The common workload and endpoint-only health checks support a comparative output-timing experiment, not an isolated protocol-load study, an exchange-by-exchange loss analysis, a continuous readiness guarantee, or exposure-time accuracy claims. See `ntp_method.md` for formulas, gates, exact source archive and reproduction commands.

Evidence files for each audited record: `timing-armed.json`, `telemetry.json`, `health-after.json`, and `result.json`; campaign baselines: `health-before.json`, `nodes.json`, and `settings.json`. Physical timing conclusions require `digital.csv`/`capture.sal` and the report's edge analysis.
