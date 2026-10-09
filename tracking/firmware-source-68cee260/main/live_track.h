/*
 * live_track.h -- LIVE_TRACK mode: on-node centroid extraction pipeline.
 *
 *   capture native Y8 at full requested resolution
 *     -> scan exact luminance with the batched RLE centroid detector
 *     -> emit IRP1 centroid packet over UDP
 *
 * No JPEG encode/decode or image transmission exists in this path. No image
 * leaves the node; only compact centroid data. This is the thesis's
 * "smart feature sensor" design (ESP32_S3_LOW_LATENCY_MARKER_PIPELINE.md).
 */
#ifndef LIVE_TRACK_H
#define LIVE_TRACK_H

#include <stdbool.h>
#include <stdint.h>

#include "camera_capture.h"
#include "centroid_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the LIVE_TRACK loop at the requested target FPS (paced; the actual
 * rate is bounded by exposure + readout + detection time). Uses the given
 * detector config (threshold / area bounds). */
bool live_track_start(uint32_t target_fps, cam_res_t res, const centroid_cfg_t *cfg);

/* Stop the LIVE_TRACK loop (leaves the camera running for a mode switch).
 * Returns only after the task has released its camera framebuffer. A false
 * result means the caller must not deinitialise/reconfigure the camera. */
bool live_track_stop(void);

/* Whether the loop is running. */
bool live_track_running(void);
/* Detect during frame readout: 0 off, 1 on (default), 2 on + verify against
 * the full-frame detector. Volatile A/B knob. */
void live_track_set_row_stream(unsigned mode);
unsigned live_track_row_stream(void);

typedef struct {
    bool running;
    uint32_t target_fps;
    uint64_t started_us;
    uint32_t frames;
    uint32_t frames_acquired;
    uint32_t markers_total;
    uint32_t capture_failures;
    uint32_t detector_overflows;
    /* Frames whose marker list was truncated at IRP1_MAX_MARKERS. */
    uint32_t marker_truncations;
    /* Brightest pixel the detector was handed (max since start / last frame). */
    uint8_t  frame_peak_luma;
    uint8_t  frame_peak_last;
    /* Frames scanned, and how many held a pixel at/above threshold. */
    uint32_t frames_scanned;
    uint32_t frames_above_threshold;
    uint32_t transport_drops;
    uint32_t cadence_slots;
    uint32_t cadence_skipped;
    uint32_t trigger_failures;
    uint32_t source_intervals;
    uint64_t source_interval_total_us;
    uint32_t source_interval_us;
    uint32_t source_interval_min_us;
    uint32_t source_interval_max_us;
    uint32_t cadence_phase_us;
    uint32_t cadence_phase_max_us;
    uint32_t capture_wait_us;
    uint32_t capture_wait_max_us;
    /* FREX trigger: slot -> write start (scheduling) and write duration
     * (sensor lock + SCCB). Exposure starts when the write lands. */
    uint32_t trigger_start_late_max_us;
    uint32_t trigger_start_late_250us;
    uint32_t trigger_write_min_us;
    uint32_t trigger_write_max_us;
    uint32_t trigger_write_slow_250us;
    /* Row streaming: frames detected during readout / streams discarded
     * because the grabbed frame was not the streamed one. */
    uint32_t stream_frames;
    uint32_t stream_fallbacks;
    uint32_t stream_verified;
    uint32_t stream_mismatches;
    uint32_t detection_us;
    uint32_t detection_max_us;
    uint32_t correction_us;
    uint32_t correction_max_us;
    uint32_t send_us;
    uint32_t send_max_us;
    /* Local software capture-start to task milestones; not exposure/network
     * delivery latency. Separate from the wire's saturating 16-bit fields. */
    uint32_t age_samples;
    uint32_t age_invalid;
    uint32_t capture_ready_us;
    uint32_t capture_ready_max_us;
    uint32_t capture_send_start_us;
    uint32_t capture_send_start_max_us;
    uint64_t capture_ready_total_us;
    uint64_t capture_send_start_total_us;
} live_track_stats_t;

live_track_stats_t live_track_get_stats(void);
void live_track_print_stats(void);

#ifdef __cplusplus
}
#endif
#endif /* LIVE_TRACK_H */
