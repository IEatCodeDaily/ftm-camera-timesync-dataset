# Noise forensics — 9 September 2026

## Conclusion

The disappearance of late transitions after the user reset the boards, with wiring reported unchanged, supports a state-dependent problem. It does **not** identify software as the sole cause. The recorded one-sample, sometimes simultaneous multi-channel excursions are compatible with acquisition corruption or analog threshold/common-ground disturbances as well as a state-related output problem. They are not the shape expected from ordinary 10-us camera strobes. No evidence here justifies blaming a permanent cable disconnection or asserting a firmware fix.

This audit made no hardware or source changes. Statistics are reproducible with `noise_forensics.py`; full values are in `noise_forensics_metrics.json`. Paths below are relative to this report directory unless prefixed with the project root.

## Direct waveform findings

| Static record | Transitions D0/D1/D2/D3 | Character of late excursions |
|---|---|---|
| Pre-reset 16 MS/s LOW | 24 / 0 / 2554 / 2714 | All 2646 complete HIGH excursions were one sample (62.5 ns). Of 2948 transition rows, 2308 changed two channels and 18 changed three simultaneously. |
| Pre-reset 16 MS/s HIGH | 1 / 1 / 1 / 64305 | D0–D2 have startup only. D3 has 32152 complete LOW excursions; 29358 (91.3%) are one sample, 2634 two samples and 160 three samples. |
| Pre-reset 8 MS/s LOW | 1 / 1 / 1547 / 54495 | D2 has 773 one-sample excursions. D3 has 27247 excursions: 27196 one sample and 51 two samples. |
| Pre-reset 8 MS/s HIGH | 1 / 1 / 1 / 12911 | D3 has 6455 excursions, 5669 (87.8%) one sample; some longer events also occur. |
| Post-reset 16 MS/s LOW | 0 / 0 / 0 / 0 | No transitions. |
| Post-reset 16 MS/s HIGH | 1 / 1 / 1 / 1 | Simultaneous startup transition only. |
| Post-reset 8 MS/s LOW and HIGH | 1 / 1 / 1 / 1 each | Simultaneous startup transition only; LOW begins HIGH then falls. |

Evidence: `new-acquisition/20260909-063944-static/` and `new-acquisition/20260909-064741-static/`, all four `digital.csv` and `command.json` files. Static commands report requested pad levels on each node, but these are instantaneous readbacks, not continuous independent voltage measurements.

The post-reset records have zero transition rows later than 1 ms. Their startup transition occurs exactly at sample 2048: 128 us at 16 MS/s or 256 us at 8 MS/s. This rate-scaled boundary is a strong acquisition-start signature already observed in the previous day's static control. The many later pre-reset transitions are spread across sample indices modulo 2048; the most frequent residue has only 52 of 64305 rows in the 16-MS/s HIGH record and 7 of 2948 in LOW. Thus a simple repeating 2048-sample boundary corruption pattern is not observed. This limited check does not exclude other USB/acquisition patterns.

## Timeline and confounding

The noisy five-second FTM pilot `20260909-063251-campaign/00-00-mcpwm-ftm` already has 1562/7/3178/3681 raw rises versus five qualified pulses per channel. Consequently, the later node-1 OTA cannot explain the onset of all noise.

The first 60-second campaign remains compromised: MAC loses ten qualified D3 pulses; TSF and FTM lose multiple qualified pulses on several channels. Frozen has only 11 qualified D1 pulses and six matched D1 pairs. Its `health-after.json` proves a mixed fleet: node 1 is newly rebooted on `507f8d74b8d34447`, while nodes 0/2/3 remain on `fdb0d3b4ab514a2d`. The separate OTA evidence is `E:/Projects/wireless-ir-mocap/test-output/ota-fleet/1788910716585756100/node1/verified.json`. Concurrent control invalidates frozen as a method comparison; it is not a reproducible ownership-bug test.

After the user reset, static controls are quiet and the short `20260909-064935-campaign` pilots are reported clean: NTP-style has five raw/qualified pulses on each channel; FTM has six raw and five complete per channel, with a boundary-incomplete pulse rather than rejected glitches. These five-second checks establish short-term signal recovery only. The isolated main campaign is separate evidence and must supply sustained validation.

## Source ownership and limits

The source snapshot `firmware/3939b066398cec36/main/strobe_gpio.c` reserves GPIO41 for 1-Hz output while `s_mode == STROBE_1HZ` or the load monitor is active. MCPWM teardown deletes the generator and calls `configure_pin()` to reset the pad and drive LOW. Static diagnostics require OFF/no 1-Hz task. However `strobe_gpio_fire()` suppresses camera pulses only in 1HZ/load-monitor states; OFF alone does not prohibit a caller from pulsing the pin. A camera operation from another control path can therefore violate the assumed static condition unless camera activity is independently excluded. This is an ownership risk, not proof it caused these nanosecond excursions.

The selected-source archive `firmware/3939b066398cec36/` corresponds to the installed runtime build `507f8d74b8d34447`: its ELF SHA256 starts with the runtime identity and its binary SHA256 is `80a9b87dfc7aee92c5f3e9ccb707feaaf9616df7a31d161c8ce31ca010f592a1`. The folder label comes from a different image-validation hash, as documented in `ntp_method.md`. This links the build-session source snapshot to the executable used in the controlled comparison; the archive is not a complete standalone build distribution. Existing `gpio41_audit.md` documents the broader ownership and board-routing investigation.

## Hypotheses, weighted cautiously

- **Software/peripheral state:** reset recovery and concurrent control make this plausible. Strobe scheduling jitter alone cannot explain huge counts of one-sample excursions on static pins. A stuck peripheral route or ownership race remains unproven.
- **Acquisition path:** exact startup sample2048 is directly supported. Later single-sample/multi-channel events make this plausible, but no recurring2048-sample signature was found and original `.sal` files were not independently decoded against CSV.
- **Electrical/threshold behavior:** unchanged wiring reduces support for a newly disconnected wire; it does not exclude state-dependent ground noise, output contention, supply activity, or a marginal analyzer input. Digital CSV cannot distinguish these from corruption.

To distinguish causes on recurrence, preserve the problematic state and compare simultaneous oscilloscope/second-analyzer observation, pad/peripheral ownership diagnostics, and static controls with exclusive camera/strobe control. Resetting first removes the state needed for causal isolation. Until then, describe the observation as reset-associated recovery, retain the corrupted trials, and base timing conclusions on the subsequent clean isolated records.

## Final acquisition state

Nine clean 60-second source recordings completed in `20260909-065110-campaign` before an analyzer ReadTimeout. A five-second FTM retry succeeded, but further attempts at 60 seconds/16 MS/s, 30 seconds/16 MS/s and 60 seconds/8 MS/s timed out. The final `20260909-070956-static` controls again showed no transitions after the acquisition-start artifact; 16 MS/s LOW was completely constant. Sustained-capture timeouts therefore persisted while the earlier late static-pin spikes were absent. These observations do not establish a common cause. No additional board reset occurred during these retries, and all endpoint health identities remained `507f8d74b8d34447`. See `session_log.json` and the report for final sample counts.
