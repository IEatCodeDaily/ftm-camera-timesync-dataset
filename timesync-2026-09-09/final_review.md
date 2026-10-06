# Final bounded methodological review

Reviewed `build_report.py`, `analyze.py`, current `analysis/run_metrics.json`, `noise_forensics.md`, firmware identity artifacts and continuation settings on 9 September 2026 while the final captures were still running. This reviews the builder, not the stale PDF. No hardware, manuscript or analysis source was changed.

## Assessment

The report now makes the essential distinctions correctly: no-sync means unit-slope/unshifted local output while FTM traffic continues; frozen FTM means affine holdover after synchronization; NTP-style is explicitly a private latest-sample four-timestamp estimator, not an RFC client. It preserves acquisition failures, concurrent firmware contamination, noisy conditional subsets, non-independent pulses and cadence failures. The new block-interruption paragraph correctly explains why a failed acquisition is replaced rather than silently omitted. No performance-based outlier deletion was found in the current analysis.

## Actionable issues before delivery

1. **Correct the stale source-provenance caveat in `noise_forensics.md`.** `firmware/3939b066398cec36/image-info.txt` explicitly reports ELF/runtime ID `507f8d74b8d34447`; `sha256.json` reports ELF SHA `507f8d74...` and binary SHA `80a9b87d...`, matching node-1 OTA verification. The 3939 label is the image validation hash, not a different firmware build. Replace the suggestion that the reviewed snapshot cannot be associated with 507f. Preserve the narrower limitation: selected source files and matching executable are archived, but this is not a complete rebuildable ESP-IDF distribution.

2. **Make the primary eligibility rule match its narrative.** `controlled` and the plot select campaign IDs and absence of `error`, but do not reject `INVALID: firmware changed during campaign`, missing raw data, or insufficient/full-fleet inconclusive states. Current reviewed controlled records are clean, so this is a latent claiming risk as remaining captures arrive. Derive one common explicit eligibility field and use it in builder and figures. Require actual raw data, matching expected runtime identity, sufficient matched data and valid acquisition. Keep clean cadence/completeness failures in the primary results: removing them would favor methods with dropped cycles. If a future glitchy run is included, make the headline explicitly conditional; otherwise retain it only in the diagnostic inventory.

3. **Freeze and reconcile the final matrix before final rendering.** Continuation settings are `[ftm, mac, tsf, frozen, ftm, ntp]`, representing the missing second-block FTM plus block three. The builder correctly describes this. Verify all 15 intended successful 60-second records exist, retain the original failed FTM attempt as the 16th attempted matrix recording, and keep the intervening five-second check outside the numerical matrix. Use explicit block/source labels rather than `i//5+1` in the figure; that inferred label is correct only while all preceding intended records remain present. Report the actual count if any remaining acquisition fails instead of implying three complete blocks.

4. **Update the evidence map for continuation and review artifacts.** E5 currently omits `20260909-070610-campaign`. Add it and the five-second retry path; add `ntp_hardware_audit.md` and `noise_forensics.md` to the evidence map because the narrative relies on them. The analysis manifest currently hashes result/settings only inside the `digital.csv` branch, so failed-acquisition JSON/settings are not covered by that manifest. Include failure evidence in a provenance manifest or describe the narrower manifest scope.

5. **Clarify edge-accounting field semantics.** `unclosed_or_initial_high_rises = raw - qualified - rejected` counts an observed rise with no closing fall; initial HIGH is never added to `raw`, so the field name incorrectly suggests initial-HIGH accounting. Rename it to unclosed observed rises, or track initial HIGH separately. This does not change the present phase estimates. The existing statement that initial HIGH is not an observed edge is correct.

6. **Keep delivered-pulse limitations prominent with extrema.** The current Table 4b and prose correctly retain missed-cycle evidence. Preserve that immediately next to the FTM/NTP comparison headline in the finished report; finite extrema apply to delivered, width-qualified, matched pulses, not every intended event. Do not convert the arbitrary no-sync initial phase into an improvement factor. Adding signed-mean ranges to Table 4 would make bias versus SD easier to compare without opening the CSV, but is optional because the text and per-node export distinguish them.

7. **Fix a minor historical count range.** The three repaired historical MCPWM captures include a per-node successful-health-request count of 233; apparatus prose says approximately 234–239. Use 233–239 or simply approximately four successful requests per second. This is minor and does not affect timing conclusions.

## Claims that are defensible as written

- Descriptive implementation-specific comparison, with no universal NTP/FTM ranking or exposure claim.
- Sub-microsecond historical SD and below-2-us historical finite maximum, explicitly tied to the tested firmware and records.
- Reset-associated recovery of signal quality, with software, acquisition and electrical explanations left unresolved.
- Eight-hour readiness failure followed by reboot/arming recovery, without an invented root cause or permanent fix.
- Width-qualified pilot statistics are conditional and cannot validate corrupted raw signal quality.
- The shared MCPWM path is an experimental control, while common missed cycles prevent attributing delivery failures uniquely to an estimator.

No per-pulse inferential statistics are needed for this small, serial, shared-reference dataset. A larger claim would require longer and independent apparatus sessions; adding naive confidence intervals would not repair that design limitation.

## Resolution audit after sustained acquisition failures

The final builder and current `analysis/run_metrics.json` are consistent with **nine successful 60-second matrix recordings**, not fifteen: one live FTM and two each MAC, NTP-style, TSF and frozen FTM. This is 540 seconds of successful matrix acquisition, split across separate recordings. Four subsequent/associated FTM acquisition errors are inventoried: original block-two 60-second failure, continuation 60-second/16-MS/s failure, 30-second/16-MS/s failure and 60-second/8-MS/s failure. No continuation success enters the primary analysis. The five-second retry is outside the primary selection. The report explicitly states the incomplete target and single-trial new FTM limitation; historical repetitions are kept separate.

Independent arithmetic from the nine selected JSON entries:

| Source | Runs | SD min-max (us) | Largest individual p95 abs (us) | Largest observed abs (us) | Gaps >1.5s D0/D1/D2/D3 |
|---|---:|---:|---:|---:|---|
| Live FTM | 1 | 0.607–0.749 | 1.563 | 1.688 | 0/0/0/0 |
| Frozen FTM | 2 | 0.539–0.800 | 1.501 | 1.937 | 1/1/0/1 |
| MAC/no-sync | 2 | 6.589–105.094 | 186878.188 | 186896.250 | 0/1/0/1 |
| NTP-style | 2 | 2322.845–8163.658 | 15239.063 | 55503.062 | 1/0/0/0 |
| AP TSF | 2 | 13.835–28.740 | 60.187 | 182.188 | 0/1/0/1 |

The headline conversion to **55.503 ms** for NTP-style and **1.688 us** for live FTM is correct. These are unequal-repetition, finite delivered-pulse extrema; no new universal or guaranteed bound follows.

Previous actionable items are resolved for the delivered scope: firmware source/executable identity caveat corrected; one `primary_eligible` field controls tables and plot; corrupt, mixed-build and insufficient runs are excluded while clean cadence failures remain; block labels come from saved run identity; failure manifests are included; edge-accounting field renamed; historical health range corrected to 233–239; evidence map covers new-acquisition, noise audit and release evidence. Current selection verifies all four after-build identities equal the fixed runtime image.

No remaining methodological blocker was found for delivery **as an explicitly incomplete experimental report**. The intended balanced replication remains unfinished because sustained acquisition failed, and that limitation must remain visible in the final summary. The final PDF still needs the separate render/layout check; this resolution audited the builder and data, not the rendered artifact.
