/*
 * live_track.c -- native-luminance on-node centroid tracking.
 *
 * Tracking deliberately does not share the UVC/JPEG pipeline.  The OV5640
 * emits one Y8 byte per pixel. Uncapped mode uses a qualified two-buffer latest
 * queue; synchronized mode uses one FREX-triggered framebuffer. The core-1
 * detector leases one completed framebuffer at a time. Only compact IRP1
 * centroid telemetry leaves the node.
 * This avoids JPEG work, compression-dependent displacement, and the former
 * full-frame PSRAM-to-PSRAM copy on every capture.
 */
#include "live_track.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_private/wifi.h"
#include "sdkconfig.h"
#include <string.h>

#include "camera_capture.h"
#include "camera_frex_sync.h"
#include "centroid_packet.h"
#include "intrinsic_store.h"
#include "clock_model.h"
#include "strobe_clock.h"
#include "node_config.h"
#include "task_cores.h"
#include "wifi_setup.h"
#include "mode_ctrl.h"

static const char *TAG = "live_track";

static TaskHandle_t   s_task = NULL;
static volatile bool  s_running = false;
static uint32_t       s_fps = 0;
static cam_res_t      s_res = CAM_RES_QVGA;
static centroid_cfg_t s_cfg;
static intrinsic_runtime_t s_intrinsic;
/* Core 1 owns the hot-path copy and publishes coherent snapshots for the
 * core-0 command server. A short critical section around one struct copy is
 * cheaper and more truthful than exposing torn 64-bit fields as volatile. */
static live_track_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static void stats_publish(const live_track_stats_t *stats)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats = *stats;
    portEXIT_CRITICAL(&s_stats_lock);
}

static live_track_stats_t stats_snapshot(void)
{
    live_track_stats_t stats;
    portENTER_CRITICAL(&s_stats_lock);
    stats = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    return stats;
}

/* A stopped task can remain blocked in esp_camera_fb_get() for several
 * seconds. A session token prevents that old task from mutating the next run
 * after s_running becomes true again. Mode control also waits for task exit
 * before allowing camera deinit/reconfiguration. */
static volatile uint32_t s_run_generation;

/* 1 microsecond is 1,000,000 picoseconds. Keep the scheduler and the FTM
 * affine model in the same units. */
static inline uint64_t us_to_ps(uint64_t us) { return us * 1000000ULL; }
static inline uint64_t ps_to_us(uint64_t ps) { return ps / 1000000ULL; }
typedef struct { int64_t mac_us; int64_t timer_us; } tracking_clock_pair_t;

/* Bridge the FTM/MAC model to esp_timer using the strobe path's pairing,
 * rollover extension, and freshness rule. */
static bool sample_tracking_clock(const clock_model_t *model, bool reference,
                                  tracking_clock_pair_t *pair)
{
    int64_t best_span = INT64_MAX, best_timer = 0;
    uint32_t best_mac = 0;
    for (int i = 0; i < 8; ++i) {
        int64_t before = esp_timer_get_time();
        uint32_t raw = esp_wifi_internal_get_mac_clock_time();
        int64_t after = esp_timer_get_time();
        if (after - before < best_span) {
            best_span = after - before;
            best_timer = before + best_span / 2;
            best_mac = raw;
        }
    }
    /* Anchor the 32-bit MAC wrap to NOW, not to the model's fit epoch.
     * ref_local_ps is the epoch of the last FTM fit and can sit in this node's
     * future (measured: -122.8 s on node 0), which made the guard below reject
     * every sample forever: live_track spun in wait_next_slot() and published
     * zero centroids while reporting running=1. esp_timer shares the MAC
     * clock's domain, so the local reading is always a valid wrap anchor.
     * The staleness check still uses the model epoch - that is its real job. */
    uint64_t anchor = (uint64_t)best_timer;
    int64_t extended = strobe_mac_extend(best_mac, anchor);
    if (extended < 0 || best_span > 10) return false;
    if (!reference) {
        int64_t model_age_us =
            extended - (int64_t)(model->fit_local_ps / 1000000ULL);
        if (model_age_us < 0 || model_age_us > 30000000) return false;
    }
    pair->mac_us = extended;
    pair->timer_us = best_timer;
    return true;
}
/* The FTM reference node has no model of its own and never will: a node cannot
 * range itself, so `clock_model_get().valid` stays false on the responder
 * forever. Its local clock IS the hub clock by definition, which is exactly the
 * identity model - slope 1, offset 0.
 *
 * Without this the paced loop at wait_next_slot() spins on `!model.valid` and
 * never grabs a frame, which cost the fleet its responder's camera entirely
 * (measured: 0.00 fps paced, while the same node ran 38.07 fps unpaced and
 * served 272 UDP preview frames - the camera was never the problem).
 *
 * strobe_gpio.c already encodes this rule as `!reference && !m.valid`; this
 * keeps live_track consistent with it rather than adding a second convention. */
static clock_model_t tracking_clock_model(void)
{
    clock_model_t model = clock_model_get();
    if (!model.valid && wifi_is_master()) {
        model.valid = true;
        model.slope = 1.0;
        model.offset_ps = 0.0;
        model.drift_ppm = 0.0;
    }
    return model;
}

static inline uint16_t clamp_u16(uint64_t value)
{
    return (uint16_t)(value > UINT16_MAX ? UINT16_MAX : value);
}

/* Wait for the next absolute hub-clock slot.  Recomputing the slot from
 * "now" after every capture/process/send cycle means an overrun advances to
 * the next viable cadence boundary instead of immediately issuing a late
 * capture. */
static bool session_is_current(uint32_t generation)
{
    return s_running && generation == s_run_generation;
}

static bool wait_next_slot(uint64_t period_ps, uint64_t *last_slot_index,
                           uint64_t *slot_local_out,
                           uint32_t generation, live_track_stats_t *stats)
{
    while (session_is_current(generation)) {
        clock_model_t model = tracking_clock_model();
        tracking_clock_pair_t pair;
        if (!model.valid || !sample_tracking_clock(&model, wifi_is_master(), &pair)) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        uint64_t now_hub_ps = strobe_model_to_hub_ps(
            us_to_ps((uint64_t)pair.mac_us), model.slope, model.offset_ps);
        uint64_t slot_index = now_hub_ps / period_ps + 1ULL;
        uint64_t slot_hub_ps = slot_index * period_ps;
        uint64_t slot_local_us = (uint64_t)strobe_hub_to_timer_us(
            slot_hub_ps, pair.mac_us, pair.timer_us,
            model.slope, model.offset_ps);
        int64_t wait_us = (int64_t)slot_local_us - esp_timer_get_time();
        uint64_t period_us = ps_to_us(period_ps);
        if (wait_us <= 0 || wait_us > (int64_t)(period_us * 2ULL + 1000000ULL)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (*last_slot_index && slot_index > *last_slot_index + 1ULL) {
            stats->cadence_skipped +=
                (uint32_t)(slot_index - *last_slot_index - 1ULL);
        }
        *last_slot_index = slot_index;
        /* Hardware-timed trigger: arrive its lead early; the alarm fires at
         * the slot itself. */
        const int64_t lead_us = frex_sync_lead_us();
        if (wait_us > 2000 + lead_us) {
            vTaskDelay(pdMS_TO_TICKS((wait_us - 1500 - lead_us) / 1000));
        }
        while (session_is_current(generation) &&
               (int64_t)slot_local_us - esp_timer_get_time() > lead_us) { }
        if (!session_is_current(generation)) return false;
        stats->cadence_slots++;
        *slot_local_out = slot_local_us;
        return true;
    }
    return false;
}

/* A start can latch the DVP capture into cutting every ~3rd frame short
 * (measured 2026-09-28: ~1 start in 6, trigger->VSYNC timing and all sensor
 * registers identical to a good start). Re-applying LIVE_TRACK clears it
 * every time (5/5, 0.4 s), so the node heals itself instead of streaming at
 * 2/3 rate for the whole session.
 * ponytail: rate-limited restart, not a root fix; find the latch to drop it. */
/* Bad sessions fail 1-11 frames/s; good ones ~0 after the start handover. */
/* The latch is a random phase picked at each sensor stream start (1 start in
 * ~8; a standby cycle at +1 s latched just as often, 10/120). Re-phasing with
 * a 0x3008 software-standby cycle cured 7/7 bad sessions, no poke 0/4, so a
 * bad window first re-phases in place (ms) and only falls back to a full
 * LIVE_TRACK restart when the new phase is bad too. */
#define HEAL_WINDOW 64U /* ~2 s at 30 fps; bad sessions fail 10-50 per 4 s */
#define HEAL_FAILS 4U /* good sessions: 0 after the handover window */
#define HEAL_MIN_GAP_US 10000000LL
static int64_t s_last_heal_us;
static uint32_t s_heals;
static uint32_t s_heal_fps;
static uint32_t s_rephases;
static bool rephase_sensor(void)
{
    ++s_rephases;
    bool ok = camera_sensor_register(true, 0x3008, 0x42) == 0;
    vTaskDelay(pdMS_TO_TICKS(50));
    return camera_sensor_register(true, 0x3008, 0x02) == 0 && ok;
}
static void heal_task(void *arg)
{
    /* A stop bumps the generation: never resurrect a session the host ended.
     * ponytail: check-then-apply window is microseconds; the host's stop
     * readback would still catch it. */
    if ((uint32_t)(uintptr_t)arg != s_run_generation) { vTaskDelete(NULL); return; }
    uint32_t fps = s_heal_fps;
    ESP_LOGW(TAG, "capture failing; re-applying LIVE_TRACK (heal %" PRIu32 ")", s_heals);
    if (!mode_ctrl_apply(STROBE_LIVE_TRACK, (uint16_t)fps))
        ESP_LOGE(TAG, "self-heal restart failed");
    vTaskDelete(NULL);
}
static bool heal_due(uint32_t attempts, uint32_t fails, int64_t now_us)
{
    return attempts >= HEAL_WINDOW && fails >= HEAL_FAILS &&
        (!s_last_heal_us || now_us - s_last_heal_us > HEAL_MIN_GAP_US);
}

/* Row streaming: detect while the sensor is still reading the frame out.
 * cam_hal publishes how many rows of the frame being filled are final, so the
 * detector runs under the ~27 ms readout instead of after it. The streamed
 * result is used only when the grabbed frame is provably the one streamed
 * (same buffer, same start stamp); anything else falls back to a full-frame
 * detect on the grabbed pixels, so a mismatch costs time, never correctness. */
extern bool cam_hal_frame_progress(uint32_t *seq, const uint8_t **buf,
                                   size_t *len, int64_t *start_us);
static volatile uint8_t s_row_stream = 1; /* 0 off, 1 on, 2 on + verify */
void live_track_set_row_stream(unsigned mode) { if (mode <= 2) s_row_stream = (uint8_t)mode; }
unsigned live_track_row_stream(void) { return s_row_stream; }

typedef struct {
    bool active, complete;
    uint32_t seq;
    const uint8_t *buf;
    int64_t start_us;
    uint16_t rows;
    uint8_t peak;
} row_stream_t;

static void row_stream_abort(row_stream_t *rs)
{
    if (rs->active) centroid_stream_end(NULL, 0);
    rs->active = rs->complete = false;
}

/* Feed every row that has landed. False: the buffer was restarted. */
static bool row_stream_pump(row_stream_t *rs, uint16_t width, uint16_t height)
{
    uint32_t seq; const uint8_t *buf; size_t len; int64_t start;
    if (!cam_hal_frame_progress(&seq, &buf, &len, &start)) return true;
    if (seq != rs->seq || buf != rs->buf) return false;
    size_t avail = len / width;
    if (avail > height) avail = height;
    while (rs->rows < avail) {
        const uint8_t *row = rs->buf + (size_t)rs->rows * width;
        if (!centroid_stream_row(rs->rows, row)) return false;
        /* Same stride-4 brightness census the full-frame path takes, done
         * here while the row is still cache-hot. */
        for (uint16_t x = 0; x < width; x += 4)
            if (row[x] > rs->peak) rs->peak = row[x];
        rs->rows++;
    }
    rs->complete = rs->rows == height;
    return true;
}

/* Stream the frame triggered at trig_us until it is complete or deadline_us
 * (kept ahead of the next slot so the next trigger is never late). */
static void row_stream_run(row_stream_t *rs, int64_t trig_us, int64_t deadline_us,
                           uint16_t width, uint16_t height)
{
    while (esp_timer_get_time() < deadline_us) {
        if (!rs->active) {
            uint32_t seq; const uint8_t *buf; size_t len; int64_t start;
            if (cam_hal_frame_progress(&seq, &buf, &len, &start) && start > trig_us &&
                centroid_stream_begin(width, height, &s_cfg)) {
                *rs = (row_stream_t){ .active = true, .seq = seq, .buf = buf,
                                      .start_us = start };
            } else { vTaskDelay(1); continue; }
        }
        if (!row_stream_pump(rs, width, height)) { row_stream_abort(rs); return; }
        if (rs->complete) return;
        vTaskDelay(1);
    }
}

static void loop_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    uint16_t width = 0, height = 0;
    camera_get_framesize(&width, &height);
    centroid_t centroids[IRP1_MAX_MARKERS];
    const uint64_t period_ps = s_fps ? 1000000000000ULL / s_fps : 0;
    uint64_t last_slot_index = 0;
    uint64_t slot_local_us = 0;
    uint64_t last_capture_us = 0;
    live_track_stats_t stats = stats_snapshot();
    uint32_t win_attempts = 0, win_fails = 0, win_index = 0; /* self-heal window */
    bool rephased = false; /* last bad window already tried a re-phase */
    row_stream_t rs = {0};
    const int64_t period_us = (int64_t)(period_ps / 1000000ULL);

    ESP_LOGI(TAG,
             "native Y8 loop: %ux%u threshold=%u cadence=%s core=%d",
             width, height, s_cfg.threshold,
             s_fps ? "shared-clock" : "maximum", FTMCS_CAMERA_CORE);

    while (session_is_current(generation)) {
        if (period_ps) {
            if (!wait_next_slot(period_ps, &last_slot_index, &slot_local_us,
                                generation, &stats)) break;
            if (!session_is_current(generation)) break;
        }

        /* A paced request is a synchronized-exposure contract, not merely a
         * synchronized dequeue. Trigger OV5640 FREX at the shared-clock slot;
         * the pulse emitted by frex_sync_trigger() now represents commanded
         * exposure instead of the later framebuffer handoff. */
        const bool hw_trigger = period_ps && frex_sync_lead_us() > 0;
        uint64_t trig_start_us = !period_ps ? 0
            : hw_trigger ? slot_local_us : (uint64_t)esp_timer_get_time();
        if (period_ps && !frex_sync_trigger_at((int64_t)slot_local_us)) {
            stats.trigger_failures++;
            if (stats.trigger_failures == 1 ||
                (stats.trigger_failures & 7U) == 0U) stats_publish(&stats);
            continue;
        }
        if (period_ps) {
            /* Exposure starts when the request write lands. Split its
             * lateness into: reaching the write after the slot (scheduling)
             * and the write itself (sensor lock + SCCB transfer). */
            uint64_t now = (uint64_t)esp_timer_get_time();
            uint32_t start = (uint32_t)(trig_start_us - slot_local_us);
            uint32_t write = (uint32_t)(now - trig_start_us);
            if (start > stats.trigger_start_late_max_us) stats.trigger_start_late_max_us = start;
            if (start >= 250) stats.trigger_start_late_250us++;
            if (!stats.trigger_write_min_us || write < stats.trigger_write_min_us)
                stats.trigger_write_min_us = write;
            if (write > stats.trigger_write_max_us) stats.trigger_write_max_us = write;
            if (write >= stats.trigger_write_min_us + 250) stats.trigger_write_slow_250us++;
        }
        if (win_attempts >= HEAL_WINDOW) {
            int64_t now_us = esp_timer_get_time();
            /* Window 0 holds the start handover rejects; never judge it. */
            bool bad = period_ps && win_index++ > 0 && win_fails >= HEAL_FAILS;
            if (bad && !rephased) {
                rephased = true;
                if (!rephase_sensor()) ESP_LOGE(TAG, "sensor re-phase failed");
            } else if (!bad) {
                rephased = false;
            } else if (heal_due(win_attempts, win_fails, now_us)) {
                s_last_heal_us = now_us; ++s_heals;
                s_heal_fps = s_fps;
                if (xTaskCreatePinnedToCore(heal_task, "lt_heal", 8192,
                        (void *)(uintptr_t)generation, 5, NULL, tskNO_AFFINITY) != pdPASS)
                    ESP_LOGE(TAG, "self-heal task create failed");
                /* Keep looping: the restart's stop ends this session. */
            }
            win_attempts = win_fails = 0;
        }
        win_attempts++;
        size_t gray_len = 0;
        int y_stride = 1;
        int64_t capture_us = 0;
        const uint8_t *gray;
        uint32_t capture_wait;
        uint64_t frame_ready_us;
        {
            /* Always grab straight from the driver. The two-slot PSRAM ring
             * that used to serve the unpaced path delivered corrupted frames:
             * measured on the same scene and settings, the ring path peaked at
             * 4-56 while this direct grab peaked at 255 and detected markers.
             * It was added for throughput and never validated for content. */
            /* Finish streaming the previous trigger's frame: its last rows
             * land just after this slot. Same bound as the grab below. */
            if (rs.active) {
                const int64_t until = (int64_t)trig_start_us + period_us / 2;
                while (!rs.complete && esp_timer_get_time() < until) {
                    if (!row_stream_pump(&rs, width, height)) { row_stream_abort(&rs); break; }
                    if (!rs.complete) vTaskDelay(1);
                }
            }
            uint64_t grab_start_us = (uint64_t)esp_timer_get_time();
            /* Paced FREX is pipelined: this grab returns the previous trigger's
             * frame, which is ready within ~1 ms of the slot. If that frame was
             * lost, waiting longer only returns THIS trigger's frame after the
             * next slot began, the slot is skipped and the loop locks at half
             * rate. Give up by half a slot instead; the in-flight frame is
             * grabbed after the next, on-time trigger.
             * ponytail: half a slot; the ready-by bound is the FREX readout phase. */
            gray = camera_grab_gray_timeout(&gray_len, &y_stride, &capture_us,
                                            period_ps ? (uint32_t)(period_ps / 2000000000ULL) : 0);
            capture_wait = (uint32_t)((uint64_t)esp_timer_get_time() - grab_start_us);
            frame_ready_us = (uint64_t)esp_timer_get_time();
        }
        const bool streamed = rs.active && rs.complete && gray == rs.buf &&
                              capture_us == rs.start_us &&
                              gray_len == (size_t)width * height && y_stride == 1;
        const uint8_t streamed_peak = rs.peak;
        if (!streamed) {
            if (rs.active) stats.stream_fallbacks++;
            row_stream_abort(&rs);
        }
        rs.active = rs.complete = false;
        if (!gray || gray_len != (size_t)width * height || y_stride != 1) {
            if (gray) camera_release(gray);
            stats.capture_failures++;
            win_fails++;
            if (stats.capture_failures == 1 ||
                (stats.capture_failures & 7U) == 0U) stats_publish(&stats);
            if (session_is_current(generation)) vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        stats.frames_acquired++;
        if (capture_us > 0) {
            uint64_t current_capture_us = (uint64_t)capture_us;
            if (last_capture_us && current_capture_us > last_capture_us) {
                uint64_t interval64 = current_capture_us - last_capture_us;
                uint32_t interval = interval64 > UINT32_MAX
                    ? UINT32_MAX : (uint32_t)interval64;
                stats.source_intervals++;
                stats.source_interval_total_us += interval64;
                stats.source_interval_us = interval;
                if (!stats.source_interval_min_us ||
                    interval < stats.source_interval_min_us)
                    stats.source_interval_min_us = interval;
                if (interval > stats.source_interval_max_us)
                    stats.source_interval_max_us = interval;
            }
            last_capture_us = current_capture_us;
            if (period_ps && slot_local_us) {
                uint64_t phase64 = current_capture_us > slot_local_us
                    ? current_capture_us - slot_local_us
                    : slot_local_us - current_capture_us;
                stats.cadence_phase_us = phase64 > UINT32_MAX
                    ? UINT32_MAX : (uint32_t)phase64;
                if (stats.cadence_phase_us > stats.cadence_phase_max_us)
                    stats.cadence_phase_max_us = stats.cadence_phase_us;
            }
        }
        if (!session_is_current(generation)) {
            if (streamed) centroid_stream_end(NULL, 0);
            camera_release(gray);
            break;
        }

        /* Measure detector work only. In paced mode the selected completed
         * frame can wait in the ring until its cadence slot; that queue age is
         * not centroid processing time. Streamed: only the post-readout
         * remainder (component merge) is left. */
        uint64_t detect_start_us = (uint64_t)esp_timer_get_time();
        int count = streamed
            ? centroid_stream_end(centroids, IRP1_MAX_MARKERS)
            : centroid_detect(gray, width, height, &s_cfg,
                              centroids, IRP1_MAX_MARKERS);
        if (streamed) stats.stream_frames++;
        if (streamed && s_row_stream == 2) {
            /* Verify: the full-frame detector on the same pixels must agree
             * exactly. ponytail: doubles detect cost, A/B only. */
            centroid_t ref[IRP1_MAX_MARKERS];
            int ref_n = centroid_detect(gray, width, height, &s_cfg, ref, IRP1_MAX_MARKERS);
            stats.stream_verified++;
            if (ref_n != count || memcmp(ref, centroids, (size_t)count * sizeof(centroid_t)) != 0) {
                stats.stream_mismatches++;
                ESP_LOGW(TAG, "rowstream mismatch: streamed=%d full=%d", count, ref_n);
            }
        }
        /* Frame-content trace: detection can only find what the capture path
         * actually delivered, so record the brightest pixel the detector was
         * given. A bright marker in preview but a dark peak here means the
         * tracking capture, not the detector, lost the signal.
         *
         * Only scan when nothing was detected. The detector's SIMD path skips
         * dark pixels wholesale, so it never touches most of the frame, while
         * this census read every byte out of PSRAM and doubled detect time on
         * exactly the frames that were already working. A frame with an
         * accepted component reports that component's peak instead, which is
         * the same number the diagnostic cares about. */
        {
            uint8_t peak = 0;
            if (count > 0) {
                for (int i = 0; i < count; i++) {
                    if (centroids[i].peak_luma > peak) peak = centroids[i].peak_luma;
                }
            } else if (streamed) {
                peak = streamed_peak;
            } else {
                /* Nothing detected: sample every 4th pixel instead of reading
                 * all 307,200 bytes back out of PSRAM. This runs on exactly
                 * the frames where the marker is missing or too dim, which is
                 * when a diagnostic is wanted but throughput still matters -
                 * measured 16.0 ms of a 28.6 ms budget at 35 fps when it was a
                 * full scan. A marker spans several pixels, so a stride of 4
                 * cannot miss a real blob; it only rounds off the exact peak
                 * of a single stuck pixel, which no decision depends on. */
                for (size_t i = 0; i < gray_len; i += 4) {
                    if (gray[i] > peak) peak = gray[i];
                }
            }
            if (peak > stats.frame_peak_luma) stats.frame_peak_luma = peak;
            stats.frame_peak_last = peak;
            /* Per-frame brightness census. A marker present in only a fraction
             * of frames looks identical to a dim marker in cumulative totals,
             * so count how many frames actually cleared the threshold. */
            stats.frames_scanned++;
            if (peak >= s_cfg.threshold) stats.frames_above_threshold++;
        }
        uint64_t scan_end_us = (uint64_t)esp_timer_get_time();
        /* Release the leased driver fb as soon as the pixel scan completes. */
        camera_release(gray);
        if (centroid_detector_last_overflow()) {
            /* Incomplete component analysis is not a valid zero-marker
             * observation. Drop it so sequence gaps expose overload to the
             * host instead of silently poisoning tracking accuracy. */
            stats.detector_overflows++;
            stats_publish(&stats);
            if (stats.detector_overflows == 1 ||
                (stats.detector_overflows % 60U) == 0U) {
                ESP_LOGW(TAG, "centroid run workspace overflow; adjust exposure/threshold");
            }
            taskYIELD();
            continue;
        }
        if (s_intrinsic.valid) {
            int calibrated_count = 0;
            for (int i = 0; i < count; i++) {
                if (intrinsic_undistort_q4(&s_intrinsic,
                                           &centroids[i].x_q4,
                                           &centroids[i].y_q4)) {
                    centroids[i].flags |= 0x02; /* calibrated coordinate */
                    if (calibrated_count != i) centroids[calibrated_count] = centroids[i];
                    calibrated_count++;
                }
            }
            count = calibrated_count;
        }
        uint64_t correction_end_us = (uint64_t)esp_timer_get_time();

        bool reference_clock = false; /* no node master; model is the authority */
        clock_model_t model = tracking_clock_model();
        bool clock_valid = model.valid;
        /* Ship the capture instant in the shared hub domain, not raw local
         * time. `capture_us` comes from the camera driver's own esp_timer
         * stamp, which is boot-relative: recorded side by side the nodes'
         * epochs differed by minutes (measured 180 s across four nodes), so
         * capture_us could not be compared between cameras at all.
         *
         * Triangulation is unaffected either way - the server groups on packet
         * arrival time (main.rs ingests host_s) - but a recording whose
         * timestamps only make sense per-node cannot be reprocessed offline,
         * which is exactly what an MCAP is for. The model already maps
         * local -> hub, and on the FTM reference node it is the identity, so
         * one conversion puts every node on one timeline.
         *
         * Falls back to the raw value when no model is valid, so a node that
         * has not yet ranged still reports something monotonic rather than 0;
         * IRP1_FLAG_CLOCK_VALID already tells the consumer which it is. */
        tracking_clock_pair_t capture_pair;
        bool clock_bridge_valid = clock_valid && sample_tracking_clock(
            &model, wifi_is_master(), &capture_pair);
        uint64_t capture_hub_us = (uint64_t)capture_us;
        if (clock_bridge_valid) {
            capture_hub_us = strobe_capture_timer_to_hub_us(
                capture_us, capture_pair.mac_us, capture_pair.timer_us,
                model.slope, model.offset_ps);
        }
        uint64_t send_start_us = (uint64_t)esp_timer_get_time();
        bool sent = centroid_packet_send(
            centroids, count, clock_bridge_valid ? IRP1_FLAG_CLOCK_VALID : 0,
            capture_hub_us,
            /* These stay in local time on purpose: they are durations measured
             * on this node, and mapping both endpoints would only add the
             * model's noise to an interval that is already correct. */
            clamp_u16(frame_ready_us > (uint64_t)capture_us
                          ? frame_ready_us - (uint64_t)capture_us : 0),
            clamp_u16(correction_end_us - scan_end_us),
            clamp_u16(scan_end_us - detect_start_us),
            reference_clock ? 0 : model.model_rev,
            reference_clock ? 0 : clamp_u16((model.resid_ns_std + 999U) / 1000U),
            /* Rolling shutter: capture_us stamps row 0, so the host needs the
             * row period to place each marker's own instant. Read live rather
             * than cached - a preview can reprogram HTS underneath tracking. */
            camera_row_period_ns());
        if (!sent) stats.transport_drops++;
        uint64_t send_end_us = (uint64_t)esp_timer_get_time();

        uint32_t detect_time = (uint32_t)(scan_end_us - detect_start_us);
        uint32_t correction_time = (uint32_t)(correction_end_us - scan_end_us);
        uint32_t send_time = (uint32_t)(send_end_us - send_start_us);
        stats.frames++;
        stats.markers_total += (uint32_t)count;
        /* A truncated frame carries the same marker count as a genuinely full
         * one, so count it explicitly rather than letting the cap hide load. */
        if (centroid_detector_last_discarded()) stats.marker_truncations++;
        stats.capture_wait_us = capture_wait;
        stats.detection_us = detect_time;
        stats.correction_us = correction_time;
        stats.send_us = send_time;
        if (capture_wait > stats.capture_wait_max_us)
            stats.capture_wait_max_us = capture_wait;
        if (detect_time > stats.detection_max_us)
            stats.detection_max_us = detect_time;
        if (correction_time > stats.correction_max_us)
            stats.correction_max_us = correction_time;
        if (send_time > stats.send_max_us)
            stats.send_max_us = send_time;
        bool age_valid = capture_us > 0 && frame_ready_us >= (uint64_t)capture_us &&
                         send_start_us >= frame_ready_us;
        uint64_t ready_age = age_valid ? frame_ready_us - (uint64_t)capture_us : 0;
        uint64_t send_age = age_valid ? send_start_us - (uint64_t)capture_us : 0;
        if (age_valid && ready_age <= UINT32_MAX && send_age <= UINT32_MAX) {
            ++stats.age_samples;
            stats.capture_ready_us = (uint32_t)ready_age;
            stats.capture_send_start_us = (uint32_t)send_age;
            stats.capture_ready_total_us += ready_age;
            stats.capture_send_start_total_us += send_age;
            if (ready_age > stats.capture_ready_max_us)
                stats.capture_ready_max_us = (uint32_t)ready_age;
            if (send_age > stats.capture_send_start_max_us)
                stats.capture_send_start_max_us = (uint32_t)send_age;
        } else {
            ++stats.age_invalid;
        }
        /* Keep exact counters local to the detector task. Cross-core command
         * readers need a coherent snapshot, but copying it under a spinlock on
         * every frame steals hot-path cycles. Eight-frame publication bounds
         * observability staleness while reducing that cost by 87.5%. */
        if ((stats.frames & 7U) == 0U) stats_publish(&stats);

        /* Maximum mode follows the camera driver's continuous latest queue. */
        if (!period_ps) taskYIELD();
        /* Detect this slot's frame while it is read out; stop 2 ms before the
         * next slot so its trigger is on time. */
        else if (s_row_stream)
            row_stream_run(&rs, (int64_t)trig_start_us,
                           (int64_t)slot_local_us + period_us - 2000, width, height);
    }
    row_stream_abort(&rs);

    if (generation == s_run_generation) {
        s_running = false;
        stats.running = false;
        stats_publish(&stats);
    }
    if (s_task == self) s_task = NULL;
    vTaskDelete(NULL);
}

bool live_track_start(uint32_t target_fps, cam_res_t res,
                      const centroid_cfg_t *cfg)
{
    if (s_running || s_task) {
        if (s_running && target_fps == s_fps && res == s_res) return true;
        if (!live_track_stop()) {
            ESP_LOGE(TAG, "previous native-Y8 task did not stop cleanly");
            return false;
        }
    }
    if (target_fps > 1000) target_fps = 1000;

    /* ponytail: always the single-buffer synchronized profile (see below);
     * start + buffering in one driver init. */
    if (!camera_start_grayscale_sync(res)) {
        ESP_LOGE(TAG, "native Y8 camera start failed; LIVE_TRACK not started");
        return false;
    }
    /* Paced tracking uses a single GRAB_WHEN_EMPTY buffer so a FREX trigger is
     * tied to one exact exposure rather than an older queued free-running
     * frame. The uncapped continuous two-buffer mode was measured against it on
     * the same scene: paced peaked at 255 and detected markers at every rate
     * from 20-60 fps, while continuous peaked at 51-133 and never detected
     * anything, because free-running grabs never let AEC converge. Keep the
     * single-buffer path for both until continuous is proven on pixels. */
    bool continuous = false;
    bool buffering_ok = continuous ? camera_config_continuous_mode() : true;
    if (!buffering_ok) {
        ESP_LOGE(TAG, "%s Y8 buffering configuration failed",
                 target_fps ? "synchronized" :
                 continuous ? "continuous" : "single-buffer");
        camera_stop();
        return false;
    }
    if (continuous) {
        ESP_LOGW(TAG, "experimental two-buffer raw-Y8 capture enabled");
    }
    if (!camera_enable_fast_tracking()) {
        ESP_LOGE(TAG, "native-Y8 DVP profile unavailable; refusing an unverified profile");
        camera_stop();
        return false;
    }
    /* The fast QVGA profile rewrites OV5640 timing and exposure bounds.
     * Reapply the persisted profile afterward so manual tracking exposure and
     * gain remain authoritative. Color-only controls are harmless in Y8. */
    if (!camera_apply_tuning()) {
        ESP_LOGE(TAG, "failed to apply tracking sensor tuning");
        camera_stop();
        return false;
    }
    /* Rewriting PLL, window and AEC bounds leaves the sensor emitting dark or
     * partially-exposed frames until its auto-exposure loop re-converges.
     * Starting capture immediately produced frames whose brightest pixel was 4
     * (pure black) on some starts and ~133 on others, from an identical scene
     * that previews at 203. Measured: exactly 4 of 8 identical starts came up
     * black on both nodes, so this is a start-time race, not exposure - no
     * setting distinguishes the good starts from the bad ones.
     *
     * Blindly discarding frames does not help, because a bad start stays bad.
     * Verify instead: sample the frames we discard, and if the sensor is still
     * emitting an all-black image after the settle window, rebuild it once.
     * A real scene always has some pixel above the black floor; a stuck sensor
     * reads 4. */
    bool sensor_live = false;
    for (int attempt = 0; attempt < 5 && !sensor_live; attempt++) {
        /* Leave as soon as a converged frame is live. Measured 2026-09-24: a
         * start is either live at frame 7 or still black at frame 36, so a
         * longer window never rescues a bad start; only a rebuild does. */
        int settle = 0;
        for (; settle < 12 && !sensor_live; settle++) {
            size_t settle_len = 0;
            int settle_stride = 0;
            int64_t settle_us = 0;
            const uint8_t *settle_frame =
                camera_grab_gray(&settle_len, &settle_stride, &settle_us);
            /* A NULL grab already waited 4 s: treat it as not live and move on,
             * so 5 attempts stay inside the 30 s command recovery lease. */
            if (!settle_frame) break;
            {
                uint8_t settle_peak = 0;
                for (size_t i = 0; i < settle_len; ++i) {
                    if (settle_frame[i] > settle_peak) settle_peak = settle_frame[i];
                }
                camera_release(settle_frame);
                /* Later frames are the converged ones; only trust the tail. */
                if (settle >= 6 && settle_peak > 16) sensor_live = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "sensor settle attempt %d: %s after %d frames",
                 attempt, sensor_live ? "live" : "black", settle);
        if (!sensor_live && attempt == 0) {
            /* Experiment: a register-level rewrite of the tracking profile
             * (~tens of ms) before paying a full driver rebuild (~1.3 s). */
            ESP_LOGW(TAG, "sensor came up black after settle; rewriting profile");
            if (!camera_enable_fast_tracking() || !camera_apply_tuning()) {
                ESP_LOGE(TAG, "tracking profile rewrite failed");
            }
            continue;
        }
        if (!sensor_live && attempt + 1 < 5) {
            ESP_LOGW(TAG, "sensor came up black after settle; rebuild %d", attempt + 1);
            camera_stop();
            /* Give the sensor time to fully power down before re-init; a
             * back-to-back restart tends to land in the same bad phase. */
            vTaskDelay(pdMS_TO_TICKS(120));
            if (!camera_start_grayscale_sync(res) || !camera_enable_fast_tracking()
                || !camera_apply_tuning()) {
                ESP_LOGE(TAG, "tracking sensor rebuild failed");
                camera_stop();
                return false;
            }
        }
    }
    if (!sensor_live) {
        ESP_LOGW(TAG, "sensor still black after rebuild; continuing anyway");
    }
    if (target_fps && !frex_sync_enable(0)) {
        ESP_LOGE(TAG, "FREX unavailable; refusing paced tracking start");
        camera_stop();
        return false;
    }

    uint16_t detector_width = 0;
    camera_get_framesize(&detector_width, NULL);
    if (!centroid_detector_prepare(detector_width)) {
        ESP_LOGE(TAG, "failed to allocate centroid workspace for width=%u",
                 detector_width);
        centroid_detector_release();
        frex_sync_disable();
        camera_stop();
        return false;
    }

    s_fps = target_fps;
    s_res = res;
    s_cfg = cfg ? *cfg : (centroid_cfg_t)CENTROID_CFG_DEFAULT();
    bool effective_hmirror = false, effective_vflip = false;
    const uint16_t profile_id = res == CAM_RES_QVGA ?
        INTR_PROFILE_TRACKING_QVGA_V1 : INTR_PROFILE_STOCK_V1;
    if (!camera_get_effective_orientation(&effective_hmirror, &effective_vflip) ||
        !intrinsic_runtime_load(res, camera_sensor_pid(), profile_id,
                                effective_hmirror, effective_vflip,
                                &s_intrinsic)) {
        s_intrinsic = (intrinsic_runtime_t) {0};
        ESP_LOGW(TAG, "no valid intrinsic for this resolution; sending distorted centroids");
    } else {
        ESP_LOGI(TAG, "applying stored intrinsic to outgoing centroid coordinates");
    }
    live_track_stats_t initial_stats = {0};
    initial_stats.running = true;
    initial_stats.target_fps = target_fps;
    initial_stats.started_us = (uint64_t)esp_timer_get_time();
    stats_publish(&initial_stats);
    uint32_t generation = ++s_run_generation;
    s_running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        loop_task, "live_track", 8192, (void *)(uintptr_t)generation, 6,
        &s_task, FTMCS_CAMERA_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create native Y8 tracking task");
        s_running = false;
        initial_stats.running = false;
        stats_publish(&initial_stats);
        centroid_detector_release();
        frex_sync_disable();
        camera_stop();
        return false;
    }
    return true;
}

bool live_track_stop(void)
{
    s_running = false;
    ++s_run_generation;
    /* A raw capture timeout can be several seconds. Do not let a subsequent
     * camera reconfiguration overlap the old DMA consumer. */
    for (int i = 0; i < 120 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_task) {
        ESP_LOGE(TAG, "stop timed out; camera framebuffer may still be leased");
        return false;
    }
    if (!centroid_detector_release()) return false;
    frex_sync_disable();
    live_track_stats_t stats = stats_snapshot();
    stats.running = false;
    stats_publish(&stats);
    ESP_LOGI(TAG, "stopped (frames=%" PRIu32 " markers=%" PRIu32 ")",
             stats.frames, stats.markers_total);
    return true;
}

/* A timed-out stop leaves s_running false while the task may still own an
 * esp32-camera framebuffer. Report that lifecycle state as running so every
 * later mode transition retries the join instead of reconfiguring underneath
 * the old consumer. */
bool live_track_running(void) { return s_running || s_task != NULL; }

live_track_stats_t live_track_get_stats(void)
{
    /* `running` is published with the session counters. In particular it
     * remains true while a stopped task is still joining, so a stats read can
     * never combine old counters with a new lifecycle flag. */
    return stats_snapshot();
}

void live_track_print_stats(void)
{
    live_track_stats_t stats = live_track_get_stats();
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t elapsed_us = stats.started_us && now_us > stats.started_us
        ? now_us - stats.started_us : 0;
    double measured_fps = elapsed_us
        ? (double)stats.frames * 1000000.0 / (double)elapsed_us : 0.0;
    double source_fps = stats.source_interval_total_us
        ? (double)stats.source_intervals * 1000000.0 /
          (double)stats.source_interval_total_us : 0.0;
    printf("track: running=%d format=Y8 target=%" PRIu32
           " measured=%.2f frames=%" PRIu32 " acquired=%" PRIu32
           " markers=%" PRIu32
           " capture_fail=%" PRIu32 " slots=%" PRIu32
           " skipped=%" PRIu32 " trigger_fail=%" PRIu32
           " detector_overflow=%" PRIu32 " truncated=%" PRIu32 " tx_drop=%" PRIu32 " peak=%u peak_last=%u"
           " bright_frames=%" PRIu32 "/%" PRIu32 " heals=%" PRIu32 " rephases=%" PRIu32 "\n",
           stats.running, stats.target_fps, measured_fps, stats.frames,
           stats.frames_acquired, stats.markers_total, stats.capture_failures, stats.cadence_slots,
           stats.cadence_skipped, stats.trigger_failures,
           stats.detector_overflows, stats.marker_truncations, stats.transport_drops,
           (unsigned)stats.frame_peak_luma, (unsigned)stats.frame_peak_last,
           stats.frames_above_threshold, stats.frames_scanned, s_heals, s_rephases);
    printf("track source: fps=%.2f interval=%" PRIu32 "/%" PRIu32 "/%" PRIu32
           " cadence_phase=%" PRIu32 "/%" PRIu32
           " us (last/min/max; phase last/max; timestamp is software capture-start, not exposure)\n",
           source_fps, stats.source_interval_us, stats.source_interval_min_us,
           stats.source_interval_max_us, stats.cadence_phase_us,
           stats.cadence_phase_max_us);
    printf("track trigger us: start_late max=%" PRIu32 " >=250=%" PRIu32
           " write min=%" PRIu32 " max=%" PRIu32 " >min+250=%" PRIu32 "\n",
           stats.trigger_start_late_max_us, stats.trigger_start_late_250us,
           stats.trigger_write_min_us, stats.trigger_write_max_us,
           stats.trigger_write_slow_250us);
    printf("track rowstream: %s streamed=%" PRIu32 " fallback=%" PRIu32
           " verified=%" PRIu32 " mismatch=%" PRIu32 "\n",
           s_row_stream == 2 ? "verify" : s_row_stream ? "on" : "off",
           stats.stream_frames, stats.stream_fallbacks,
           stats.stream_verified, stats.stream_mismatches);
    printf("track timing us: grab=%" PRIu32 "/%" PRIu32
           " detect=%" PRIu32 "/%" PRIu32
           " correct=%" PRIu32 "/%" PRIu32
           " send=%" PRIu32 "/%" PRIu32 " (last/max)\n",
           stats.capture_wait_us, stats.capture_wait_max_us,
           stats.detection_us, stats.detection_max_us,
           stats.correction_us, stats.correction_max_us,
           stats.send_us, stats.send_max_us);
    printf("track age us: ready=%" PRIu32 "/%" PRIu32 "/%.2f"
           " send_start=%" PRIu32 "/%" PRIu32 "/%.2f"
           " samples=%" PRIu32 " invalid=%" PRIu32
           " (last/max/mean; software capture-start to task milestones, not exposure or delivery)\n",
           stats.capture_ready_us, stats.capture_ready_max_us,
           stats.age_samples ? (double)stats.capture_ready_total_us / stats.age_samples : 0.0,
           stats.capture_send_start_us, stats.capture_send_start_max_us,
           stats.age_samples ? (double)stats.capture_send_start_total_us / stats.age_samples : 0.0,
           stats.age_samples, stats.age_invalid);
}
