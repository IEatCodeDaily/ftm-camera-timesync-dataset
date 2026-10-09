/*
 * camera_frex_sync.h -- OV5640 FREX Mode 2 software-triggered sync.
 *
 * The OV5640 supports a software-triggered frame exposure ("FREX Mode 2")
 * via the I2C register 0x3B08[0]. Writing 1 to this bit starts a fresh
 * exposure immediately -- no external FREX pin needed. Since all nodes share
 * the same FTM clock model, they can all trigger at the same model-aligned
 * tick, achieving sub-millisecond frame sync without any hardware modification.
 *
 * Register sequence (from the OV5640 datasheet + Linux kernel driver):
 *   1. Configure pad output enable (0x3017=0x7F, 0x3018=0xFC) -- FREX is
 *      already input by default in DVP mode.
 *   2. Program FREX exposure time (0x3B01, 0x3B04, 0x3B05) in Tline units.
 *   3. Set FREX mode (0x3B07=0x09 for strobe mode 1 / Mode 2 trigger).
 *   4. At each sync tick: write 0x3B08 = 0x01 to trigger one frame.
 *
 * The SCCB/I2C latency (~50-100 us per write) is the limiting factor for
 * cross-node alignment, but it's deterministic and far below a frame period.
 */
#ifndef CAMERA_FREX_SYNC_H
#define CAMERA_FREX_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the OV5640 for FREX Mode 2 (software-triggered exposure).
 * Call after camera_init / camera_start, before the sync loop begins.
 * The exposure_lines parameter sets the integration time in row periods
 * (Tline). 0 = the persisted tuning exposure, clamped for FREX
 * (frex_sync_tuned_exposure_lines). Returns true on success. */
bool frex_sync_enable(uint16_t exposure_lines);

/* Tuned exposure mapped to FREX Tlines: -1 (auto) and values above the cap
 * both give the cap. */
uint16_t frex_sync_tuned_exposure_lines(void);

/* Rewrite the FREX exposure from the current tuning while FREX is active, so
 * a re-tune takes effect on the next trigger. No-op when FREX is off. Caller
 * must hold the sensor mutation lock. */
bool frex_sync_reapply_exposure_ordinary_owned(void);

/* Trigger one frame exposure via I2C (write 0x3B08 = 0x01).
 * Call at each scheduled sync tick. The OV5640 starts a fresh exposure
 * immediately. fb_get() will then return the triggered frame. */
/* Returns true when the SCCB request was accepted. */
bool frex_sync_trigger(void);

/* True while grabs are externally scheduled. Used to ensure each capture
 * emits exactly one GPIO pulse at the exposure trigger rather than a second
 * pulse when the completed framebuffer is collected. */
bool frex_sync_active(void);

/* Whether an asserted FREX exposure is still outstanding.
 *
 * 1 = still pending, 0 = retired (the triggered frame has been read out),
 * -1 = unknown (readback failed, or FREX is not enabled). Callers must treat
 * -1 as "not confirmed synchronized"; it is not a synonym for 0. */
int frex_sync_request_pending(void);

/* Trigger at esp_timer instant `deadline_us`. With `cam hwtrig on` the
 * request write is started by a GPTimer alarm; the caller must reach here
 * frex_sync_lead_us() before the deadline. Off: frex_sync_trigger() now. */
bool frex_sync_trigger_at(int64_t deadline_us);
int64_t frex_sync_lead_us(void);

typedef struct {
    uint32_t ok, late, nack, timeout, busy;
    int32_t fire_min_us, fire_max_us;   /* alarm interrupt - deadline */
    int32_t done_min_us, done_max_us;   /* request write complete - deadline */
} frex_hw_stats_t;
void frex_sync_hw_set(bool on);
bool frex_sync_hw_get(void);
void frex_sync_hw_stats(frex_hw_stats_t *out, bool reset);

/* Disable FREX Mode 2: restore normal autonomous streaming. */
void frex_sync_disable(void);

#ifdef __cplusplus
}
#endif
#endif /* CAMERA_FREX_SYNC_H */
