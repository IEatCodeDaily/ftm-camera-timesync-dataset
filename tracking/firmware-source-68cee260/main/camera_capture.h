/*
 * camera_capture.h -- DVP camera bring-up and JPEG capture (node-only).
 *
 * Wraps esp32-camera for the intrinsic-calibration workflow:
 *   - a table of the known ESP32-S3 DVP pin presets;
 *   - camera_detect(): probe each preset until a sensor responds;
 *   - camera_start()/camera_stop() with a runtime frame size;
 *   - camera_grab_jpeg(): fetch one JPEG framebuffer.
 *
 * The hub build compiles this entire module out (camera is node-only).
 */
#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Calibration resolutions we store intrinsics for. */
typedef enum {
    CAM_RES_QVGA = 0,   /* 320 x 240  -- fastest preview          */
    CAM_RES_VGA = 1,    /* 640 x 480  -- main tracking baseline  */
    CAM_RES_SXGA = 2,   /* 1280 x 1024 -- high-accuracy calib    */
    CAM_RES_HD = 3,     /* 1280 x 720  -- widescreen preview      */
    CAM_RES_FHD = 4,    /* 1920 x 1080 -- maximum OV5640 video    */
    CAM_RES_COUNT
} cam_res_t;

/* Result of a detection attempt. */
typedef struct {
    bool        ok;            /* a sensor responded to at least one preset */
    int         preset;        /* 1-based preset index that worked (0 = none) */
    const char *preset_name;   /* "S3-EYE/Freenove", "XIAO Sense", ... */
    const char *sensor_name;   /* "OV5640", "OV2640", ... or "none" */
    uint16_t    pid;           /* sensor PID register */
} cam_detect_result_t;

/* Probe the configured (or all, if 0) pin presets and return which sensor
 * responds. Leaves the camera initialised on the working preset so a follow-up
 * camera_start() is fast. Safe to call repeatedly. */
cam_detect_result_t camera_detect(void);

/* Initialise the camera on the last-detected (or Kconfig-default) preset at the
 * given resolution. Returns true on success. Call after camera_detect() or as
 * the first camera call (it will detect if needed). */
bool camera_start(cam_res_t res);

/* Stop the camera and release its resources (frees PSRAM frame buffers). */
void camera_stop(void);

/* Whether the camera is currently running. */
bool camera_running(void);

/* VSYNC GPIO of the detected board pinout, -1 before detection. */
int camera_vsync_pin(void);

/* Current frame size as width/height; valid while running. */
void camera_get_framesize(uint16_t *w, uint16_t *h);

/* Grab one JPEG frame. Returns a pointer to a buffer owned by the camera
 * driver (caller must NOT free); *out_len receives the byte length. Returns
 * NULL on failure or if not running. The caller MUST release the frame with
 * camera_release() before grabbing the next one.
 *
 * If out_capture_us is non-NULL, it receives the frame's VSYNC-aligned capture
 * timestamp (esp_timer microseconds since boot), as reported by the camera
 * driver. This is the authoritative capture time used for timestamp-aware
 * reconstruction -- never use packet-arrival time instead.
 *
 * Each successful grab also fires the capture-trigger GPIO (capture_trigger.h)
 * so a logic analyzer / future strobe sees the exact grab instant. */
const uint8_t *camera_grab_jpeg(size_t *out_len, int64_t *out_capture_us);

/* Release a frame returned by camera_grab_jpeg(). */
void camera_release(const uint8_t *buf);

/* Set manual exposure level (sensor-dependent units, smaller = darker).
 * Pass < 0 to restore auto-exposure. Tablet-screen calibration needs a locked
 * exposure to beat display refresh banding. */
bool camera_set_exposure(int level);

/* Temporarily override the per-resolution persisted mount orientation. The
 * calibration workflow uses this before capture, then stores the same bits in
 * the solved intrinsic blob. Clearing the override restores NVS/default use. */
bool camera_set_orientation_override(bool hmirror, bool vflip);
bool camera_clear_orientation_override(void);
uint16_t camera_sensor_pid(void);
/* Sensor PID from the last probe (0 = none yet); safe without the sensor lock. */
uint16_t camera_detected_pid(void);
bool camera_get_effective_orientation(bool *hmirror, bool *vflip);

/* Apply a short, fixed exposure for high-rate illuminated-marker tracking. */
bool camera_set_tracking_exposure(uint16_t lines);
/* Fixed AGC gain (0-64), applied live like the exposure setter above. */
bool camera_set_tracking_gain(uint8_t gain);

/* Change OV5640 sensor-side JPEG compression while capture is running.
 * Larger values produce smaller/faster frames (valid range 0..63). */
bool camera_set_jpeg_quality(uint8_t quality);

/* Sensor tuning shared by preview, synchronized capture, and tracking.
 * exposure_lines/gain use -1 for automatic control. white_balance is
 * 0=auto, 1=sunny, 2=cloudy, 3=office, 4=home, 5=disabled. */
typedef struct {
    uint8_t jpeg_quality;
    int16_t exposure_lines;
    int8_t gain;
    uint8_t white_balance;
    int8_t brightness;
    int8_t contrast;
    int8_t saturation;
    int8_t sharpness;
} camera_tuning_t;

bool camera_set_tuning(const camera_tuning_t *tuning, bool persist);
void camera_get_tuning(camera_tuning_t *out);
bool camera_apply_tuning(void);

/* Apply the OV5640 vendor 30 FPS QVGA timing profile used by tracking. */
bool camera_enable_fast_tracking(void);

/* Change only the native-Y8 QVGA sensor DVP pixel clock while the tracking
 * profile is active. Values are clamped to the ESP32-S3's documented 40 MHz
 * camera-interface limit. Intended for qualified runtime throughput sweeps. */
bool camera_set_tracking_pclk(uint32_t requested_mhz, uint32_t *actual_mhz);

/* Select the OV5640 sensor sampling envelope for QVGA Y8. 2 preserves the
 * vendor 2x-binned/scaled path; 4 uses full-FOV 4x subsampling and a smaller
 * timing envelope while still outputting every 320x240 detector pixel. */
bool camera_set_tracking_subsample(uint8_t factor);

/* Tracking line length; 2060 = stock exposure-friendly, 1556 = fast. */
/* Live sensor state: resolved exposure rows, gain (1/16 steps), HTS, VTS. */
bool camera_read_live_exposure(uint32_t *out_rows, uint16_t *out_gain_q4,
                               uint16_t *out_hts, uint16_t *out_vts);

bool camera_set_tracking_hts(unsigned hts);
unsigned camera_tracking_hts(void);

/* Rolling-shutter row period (ns), 0 when no Y8 profile is programmed or the
 * sensor's line length cannot be read. Marker on row r is captured
 * r * row_period after the frame's capture timestamp. */
uint32_t camera_row_period_ns(void);

/* Raise the OV5640 JPEG pixel clock without changing geometry. QVGA uses the
 * fully tuned profile above; larger modes retain their driver crop/scaling. */
bool camera_enable_high_rate_stream(uint32_t requested_fps);


/* Switch the camera to native Y8 pixel format (for LIVE_TRACK and preview).
 * The sensor emits luminance only, avoiding JPEG and YUV chroma work. Use
 * camera_grab_gray() instead of camera_grab_jpeg(). */
bool camera_start_grayscale(cam_res_t res);

/* Grab one native grayscale frame. The buffer is tightly packed Y8 and
 * *out_y_stride is 1. *out_capture_us receives the
 * VSYNC-aligned capture timestamp. The caller MUST release via camera_release().
 * Returns NULL on failure / not running. */
const uint8_t *camera_grab_gray(size_t *out_len, int *out_y_stride,
                                int64_t *out_capture_us);
/* Same, but gives up after timeout_ms (0 = the camera_grab_gray default). */
const uint8_t *camera_grab_gray_timeout(size_t *out_len, int *out_y_stride,
                                        int64_t *out_capture_us, uint32_t timeout_ms);

/* Producer variant used by LIVE_TRACK's RTOS ring. The consumer emits the
 * GPIO pulse only for frames actually selected for telemetry. */
const uint8_t *camera_grab_gray_quiet(size_t *out_len, int *out_y_stride,
                                      int64_t *out_capture_us);

/* Whether the camera is currently in native Y8 mode vs JPEG. */
bool camera_is_grayscale(void);
/* Exclusive startup owner only. Drain pre-configuration frames and two fresh
 * sensor frames before publishing a reconfigured free-running preview. */
bool camera_prime_preview(void);

/* Volatile sensor-generated color bars for software-only path validation. */
bool camera_set_test_pattern(bool enabled);
/* OV5640 active-camera diagnostic; checked0x503d=0x82, volatile COLOR squares. */
bool camera_set_stationary_color_squares(void);
bool camera_set_perf_profile(bool enabled);
bool camera_set_pipeline_diagnostics(bool enabled);
bool camera_get_perf_profile(void);
void camera_print_profile(void);
bool camera_set_raw_buffer_count(unsigned count);
bool camera_set_raw_direct_dma(bool enabled);
/* cam_task priority: 0 default (24), 20..24 explicit; applies at the next camera init. */
bool camera_set_copy_prio(unsigned prio);
/* Internal bounce ring capacity experiment; preserves the JPEG path. */
bool camera_set_raw_dma_ring(unsigned kib);
/* OFF-only, volatile native QVGA Y8 EOF experiment: 0 (default), 7680 or 3840.
 * Stages reinitialization; other resolutions and direct DMA are unchanged. */
bool camera_set_raw_dma_chunk(unsigned bytes);
unsigned camera_get_raw_dma_chunk(void);
/* Exclusive diagnostic: sensor bars across all rows, restores pattern/camera OFF. */
bool camera_check_raw_pattern(cam_res_t resolution);
bool camera_set_jpeg_buffer_count(unsigned count);
bool camera_set_qvga_pclk_div(unsigned divisor);
bool camera_set_tracking_vco(unsigned mhz);
/* Paced-Y8 framebuffer count: 1 = strict sync, 2 = overlap capture/detect. */
bool camera_set_raw_sync_buffers(unsigned count);
bool camera_set_preview_vco(unsigned mhz);
bool camera_set_preview_hts(unsigned hts);
bool camera_enable_perf_preview(void);

/* Reconfigure for synchronous capture: fb_count=1, GRAB_WHEN_EMPTY so fb_get()
 * blocks tightly on the next VSYNC. Call before FRAME_SYNC mode. Returns to
 * the default (fb_count=2, GRAB_LATEST) via camera_start(). */
bool camera_config_sync_mode(void);

/* camera_start_grayscale(res) + camera_config_sync_mode() in one driver init. */
bool camera_start_grayscale_sync(cam_res_t res);

/* Restore the normal two-buffer JPEG producer after a synchronized session.
 * Native grayscale remains one-buffer because continuous raw multi-buffer
 * capture is not reliable in esp32-camera. */
bool camera_config_continuous_mode(void);

/* Diagnostic proposal: requires OFF and full camera teardown. */
bool camera_set_raw_eof_trace(bool enabled);
bool camera_print_raw_eof_trace(void);


/* Serialized, finite one-shot diagnostic; publishes no frames. */
bool camera_run_deferred_eof(unsigned wait_us);
bool camera_run_deferred_content(unsigned wait_us);
/* Requires initialized sensor and caller-held mode ownership; no writes. */
bool camera_print_sensor_registers(void);

/* Startup-only: call once before console/network/camera tasks are created. */
bool camera_sensor_lifetime_init(void);

/* Task-only ordinary mutation lease; recursive, excludes pending CIP. */
bool camera_sensor_mutation_begin(void);
void camera_sensor_mutation_end(void);
/* Raw diagnostic access under lifetime/CIP ownership; -1 if unavailable. */
int camera_sensor_register(bool write, unsigned addr, unsigned value);
/* Requires mode ownership; keeps no sensor pointer after return. */
typedef enum {
    CAMERA_CIP_MATCH = 0,
    CAMERA_CIP_RESTORE = 1,
    CAMERA_CIP_EDGE_PLUS_ONE = 2,
    CAMERA_CIP_EDGE_RETURN = 3,
} camera_cip_action_t;
bool camera_cip_command(camera_cip_action_t action);

bool camera_set_startup_vsync_only(bool enabled);
bool camera_get_startup_vsync_only(bool *enabled);
bool camera_set_jpeg_mode_requested(unsigned mode);
unsigned camera_get_jpeg_mode_requested(void);
bool camera_apply_jpeg_mode_preview(void);
bool camera_get_jpeg_mode_snapshot(unsigned *mode, uint64_t *generation, bool *ready);
#ifdef __cplusplus
}
#endif
#endif /* CAMERA_CAPTURE_H */
