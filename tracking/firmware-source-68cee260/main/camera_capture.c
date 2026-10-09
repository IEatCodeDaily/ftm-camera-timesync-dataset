/*
 * camera_capture.c -- see camera_capture.h.
 *
 * Pin presets are the four well-known ESP32-S3 DVP mappings (verified against
 * the esp32-camera examples and the board vendors). camera_detect() tries each
 * in turn: esp_camera_init() internally probes the SCCB bus and the sensor ID
 * registers, so a correct pinout yields a "Detected <sensor> camera" log and a
 * non-error return. We then read the sensor info to report the model name.
 */
#include "camera_capture.h"
#include "ota_server.h"
#include "ftmcs_camera_eof_trace.h"
#include "ftmcs_deferred_eof.h"
#include "ftmcs_startup_vsync.h"
#include "ftmcs_deferred_content.h"
#include "camera_frex_sync.h"
#include "sensor_register_snapshot.h"
#include "cip_matched.h"

// Camera subsystem always enabled on the thesis ESP32-S3 boards (see
// camera_cmd.c for why the Kconfig guard was removed).
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sensor.h"

#include "intrinsic_store.h"
#include "strobe_gpio.h"
#include "task_cores.h"
#include "recovery.h"

static const char *TAG = "camera_capture";
static int s_driver_task_core = -1;
static unsigned s_copy_prio; /* 0 = default 24 (see camera_driver_init) */
static size_t s_dma_buffer_bytes, s_dma_chunk_bytes;
/* Framebuffers for paced Y8 capture. `cam syncbufs`.
 *
 * 2 (default) lets the sensor fill frame N+1 while the detector scans N.
 * Measured at VGA, node 0, same scene:
 *
 *   bufs=1  target 25 -> 24.8 fps, grab 27.7 ms
 *   bufs=2  target 25 -> 24.6 fps, grab   70 us
 *   bufs=2  target 35 -> 34.7 fps, grab   70 us, capfail=0
 *
 * The 27.7 ms -> 70 us drop is NOT a 400x speedup: readout still costs a full
 * sensor frame, it just stops blocking because the next frame is already in
 * flight while the detector scans the current one. Verified the extra frames
 * are real and not a buffer re-delivered twice - over 20 s across 3 cameras
 * the server accepted 1049 packets at bufs=1 and 2100 at bufs=2, exactly 2.00x,
 * with mocap_centroid_sequence_gaps_total = 0 in both. The server drops any
 * packet whose sequence is not newer, so duplicates would lower that count,
 * not raise it. The sensor's own capture timestamps agree: median interval
 * 28.6 ms = 34.95 fps against a measured 34.69.
 *
 * The single buffer was kept on the theory that one trigger per buffer is
 * needed for exposure sync. Measuring the thing that actually matters - the
 * SPREAD of capture phase ACROSS cameras, since the estimator groups
 * observations within sync_window_s = 20 ms of capture timestamp - reversed
 * that (three nodes, three repeats each):
 *
 *   bufs=1 spread: 20.12, 20.26, 20.12, 20.14 ms  <- 4/4 runs miss the window
 *   bufs=2 spread:  0.02,  0.15,  0.05,  0.09 ms
 *
 * With one buffer a node that latches into the slow-grab phase captures a
 * whole slot later than its peers. With two, every node lags by the same
 * constant, and a uniform offset cancels in triangulation because each packet
 * carries its own capture timestamp rather than a slot index. So two buffers
 * are both faster and better synchronized. Set 1 only to reproduce the old
 * strict one-trigger-one-buffer behaviour. */
static unsigned s_raw_sync_buffers = 2;
static unsigned s_preview_hts, s_effective_preview_hts;
static bool s_direct_dma;
/* Direct Y8 DMA doubled QVGA tracking cadence (32 -> 64 fps) but CORRUPTS the
 * frame: captures show a 1-pixel vertical comb (alternating columns pegged at
 * the rails), not an image. The 64 fps was the sensor free-running while the
 * DMA descriptor chain mis-sampled the DVP bus, and the original qualification
 * missed it because it counted markers instead of checking pixels. Left opt-in
 * via `cam rawdma on` for further investigation; not safe as a default.
 * 2026-09-16: reverted after direct pixel comparison against JPEG preview. */
static bool s_raw_direct_dma;
static unsigned s_raw_dma_ring_kib; /* 0: resolution-selected ring. */
static unsigned s_raw_dma_chunk_requested; /* 0: original EOF geometry. */
size_t ftmcs_camera_raw_dma_chunk_bytes(unsigned width, unsigned height)
{
    return width == 320 && height == 240 ? s_raw_dma_chunk_requested : 0;
}
size_t ftmcs_camera_raw_dma_ring_bytes(unsigned width, unsigned height)
{
    /* Use incoming dimensions; s_res can still describe the previous mode. */
    unsigned kib = s_raw_dma_ring_kib ? s_raw_dma_ring_kib :
                   (width == 640 && height == 480 ? 64U : 32U);
    size_t want = (size_t)kib * 1024;
    if (s_raw_dma_ring_kib) {
        return want;  /* explicit override wins, including for A/B testing */
    }
    /* The ring is DMA-capable internal RAM, which is scarce and fragments
     * differently per board: one node in this rig tops out at a 48 KB largest
     * free block while an identical sibling offers 80 KB, so the 64 KiB VGA
     * ring failed camera init there with ESP_FAIL and Y8 VGA could not start
     * at all. Halve until it fits (measured: 32 KiB starts and tracks on that
     * board) rather than refusing to capture. */
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    while (want > 8 * 1024 && want + want / 4 > largest) {
        want /= 2;
    }
    return want;
}
bool ftmcs_camera_psram_dma(const camera_config_t *config)
{
    return s_raw_direct_dma && config->pixel_format == PIXFORMAT_GRAYSCALE;
}
void ftmcs_camera_dma_configured(size_t buffer, size_t chunk, bool direct)
{
    s_dma_buffer_bytes = buffer;
    s_dma_chunk_bytes = chunk;
    s_direct_dma = direct;
}
int ftmcs_camera_task_core(const camera_config_t *config)
{
    /* JPEG has no detector to overlap: keep its copy off the Wi-Fi core.
     * QVGA raw also regressed with core0 copy. Larger raw frames benefit from
     * overlapping copy on core0 with detection on core1. ISR/init is core1. */
    s_driver_task_core = config->pixel_format == PIXFORMAT_JPEG ||
                         (config->pixel_format == PIXFORMAT_GRAYSCALE &&
                          config->frame_size == FRAMESIZE_QVGA) ? 1 : 0;
    return s_driver_task_core;
}

/* Initialized once by app_main before console/network/camera tasks exist.
 * All camera-driver lifetime operations and sensor snapshot bytes share this
 * mutex. Never acquire the mode mutex while holding this lifetime mutex. */
static StaticSemaphore_t s_sensor_lifetime_storage;
static SemaphoreHandle_t s_sensor_lifetime_lock;
static bool s_sensor_lifetime_ready;
static bool s_sensor_teardown_unconfirmed;
static uint64_t s_sensor_generation;
static cip_state s_cip;
static cip_status camera_cip_restore_owned(void);

bool camera_sensor_lifetime_init(void)
{
    /* Startup-only, single caller; no lazy initialization in task paths. */
    if (s_sensor_lifetime_lock) return true;
    s_sensor_lifetime_lock = xSemaphoreCreateRecursiveMutexStatic(&s_sensor_lifetime_storage);
    return s_sensor_lifetime_lock != NULL;
}

static bool sensor_lifetime_take(void)
{
    return s_sensor_lifetime_lock &&
        xSemaphoreTakeRecursive(s_sensor_lifetime_lock, portMAX_DELAY) == pdTRUE;
}

static void sensor_lifetime_give(void)
{
    xSemaphoreGiveRecursive(s_sensor_lifetime_lock);
}

/* Typed ordinary mutation lease. Retain it through software/NVS changes and
 * sensor writes; recursive nesting is intentional. No mode lock is taken. */
bool camera_sensor_mutation_begin(void)
{
    if (!sensor_lifetime_take()) return false;
    if (!cip_setter_allowed(&s_cip) || s_sensor_teardown_unconfirmed) {
        sensor_lifetime_give();
        return false;
    }
    return true;
}
void camera_sensor_mutation_end(void) { sensor_lifetime_give(); }

int camera_sensor_register(bool write, unsigned addr, unsigned value)
{
    if (!camera_sensor_mutation_begin()) return -1;
    sensor_t *sensor = esp_camera_sensor_get();
    int result = -1;
    if (sensor) {
        if (write && sensor->set_reg)
            result = sensor->set_reg(sensor, (int)addr, 0xff, (int)value);
        else if (!write && sensor->get_reg)
            result = sensor->get_reg(sensor, (int)addr, 0xff);
    }
    camera_sensor_mutation_end();
    return result;
}

static esp_err_t camera_driver_deinit(void)
{
    if (!sensor_lifetime_take()) return ESP_ERR_INVALID_STATE;
    cip_status restoration = camera_cip_restore_owned();
    s_sensor_teardown_unconfirmed = true;
    s_sensor_lifetime_ready = false;
    esp_err_t result = esp_camera_deinit();
    if (result == ESP_OK) {
        (void)cip_destroyed(&s_cip, s_sensor_generation, true);
        s_sensor_teardown_unconfirmed = false;
    }
    if (result != ESP_OK && s_cip.generation == s_sensor_generation && s_cip.generation) {
        s_cip.phase = CIP_PENDING;
        s_cip.restored = false;
    }
    /* Error state is retained across failed SDK teardown and blocks reinit. */
    sensor_lifetime_give();
    if (restoration != CIP_OK || result != ESP_OK)
        ESP_LOGE(TAG, "CIP teardown restore=%d driver=%d", (int)restoration, (int)result);
    return result;
}

typedef struct {
    const camera_config_t *config;
    SemaphoreHandle_t done;
    esp_err_t result;
} camera_init_request_t;

static void camera_init_worker(void *arg)
{
    camera_init_request_t *request = arg;
    request->result = esp_camera_init(request->config);
    xSemaphoreGive(request->done);
    vTaskDelete(NULL);
}

/* esp_intr_alloc binds camera/DMA interrupts to the initialization core.
 * Moving only cam_task leaves its interrupts competing with Wi-Fi on core 0.
 * Mode control owns the camera while this bounded initialization completes. */
static esp_err_t camera_driver_init_owned(const camera_config_t *config)
{
    uint32_t guard = recovery_begin(REC_MODE, 30000);
    if (xPortGetCoreID() == FTMCS_CAMERA_CORE) {
        esp_err_t result = esp_camera_init(config);
        recovery_end(guard);
        return result;
    }
    camera_init_request_t request = {.config = config, .result = ESP_FAIL};
    request.done = xSemaphoreCreateBinary();
    if (!request.done) { recovery_end(guard); return ESP_ERR_NO_MEM; }
    if (xTaskCreatePinnedToCore(camera_init_worker, "cam_init", 8192,
                               &request, 5, NULL, FTMCS_CAMERA_CORE) != pdPASS) {
        vSemaphoreDelete(request.done);
        recovery_end(guard);
        return ESP_ERR_NO_MEM;
    }
    /* Do not return a stack-backed request while the worker still owns it.
     * The independent recovery deadline handles a wedged sensor/driver. */
    while (xSemaphoreTake(request.done, portMAX_DELAY) != pdTRUE) {
        /* Never release the stack request before the child signals completion. */
    }
    vSemaphoreDelete(request.done);
    recovery_end(guard);
    return request.result;
}

/* Parent holds ownership across the child init task. The child must never
 * take this mutex: doing so would deadlock the waiting parent. */
static esp_err_t camera_driver_init(const camera_config_t *config)
{
    if (!config || !sensor_lifetime_take()) return ESP_ERR_INVALID_STATE;
    if (s_sensor_lifetime_ready || s_sensor_teardown_unconfirmed ||
        !cip_setter_allowed(&s_cip) || s_sensor_generation == UINT64_MAX) {
        sensor_lifetime_give();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = camera_driver_init_owned(config);
    /* cam_task is created at configMAX_PRIORITIES-2, the Wi-Fi task's priority.
     * On core0 the two round-robin, and a VSYNC event served >VS_FRESH_US late
     * never starts its frame. Worst on the FTM responder (it serves every
     * initiator's burst). Measured 2026-09-26, same rig, 3x75 s: capture_fail
     * responder 2.4-3.0% / initiators 0.3-0.9% at 23, 0.0% on all four at 24;
     * capture-vs-slot offset unchanged. `cam copyprio 20..24` overrides. */
    unsigned prio = s_copy_prio ? s_copy_prio : configMAX_PRIORITIES - 1;
    TaskHandle_t cam_task = result == ESP_OK ? xTaskGetHandle("cam_task") : NULL;
    if (cam_task) vTaskPrioritySet(cam_task, prio);
    s_sensor_lifetime_ready = result == ESP_OK;
    if (result == ESP_OK) ++s_sensor_generation;
    sensor_lifetime_give();
    return result;
}

/* ---- known ESP32-S3 DVP pin presets --------------------------------------
 * Index 0 is unused (presets are 1-based to match Kconfig FTMCS_CAM_PINSET).
 * Source: esp32-camera examples + arduino-esp32 CameraWebServer camera_pins.h.
 *         -1 means "not connected" (use software reset / no PWDN).
 */
typedef struct {
    const char *name;
    camera_config_t cfg;
} pin_preset_t;

#define PRESET(name, _pwdn,_reset,_xclk,_sda,_scl,_d7,_d6,_d5,_d4,_d3,_d2,_d1,_d0,_vs,_hr,_pl) \
    { name, { \
        .pin_pwdn=(_pwdn), .pin_reset=(_reset), .pin_xclk=(_xclk), \
        .pin_sccb_sda=(_sda), .pin_sccb_scl=(_scl), \
        .pin_d7=(_d7), .pin_d6=(_d6), .pin_d5=(_d5), .pin_d4=(_d4), \
        .pin_d3=(_d3), .pin_d2=(_d2), .pin_d1=(_d1), .pin_d0=(_d0), \
        .pin_vsync=(_vs), .pin_href=(_hr), .pin_pclk=(_pl), \
        .xclk_freq_hz = CONFIG_FTMCS_CAM_XCLK_MHZ * 1000000, \
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0, \
        .pixel_format = PIXFORMAT_JPEG, \
        .frame_size = FRAMESIZE_VGA, \
        .jpeg_quality = 10, \
        .fb_count = 2, \
        .fb_location = CAMERA_FB_IN_PSRAM, \
        .grab_mode = CAMERA_GRAB_LATEST, \
    } }

/* 1 = ESP32-S3-EYE / Freenove ESP32-S3-WROOM CAM (identical mapping) */
static const pin_preset_t PRESET_S3EYE = PRESET(
    "S3-EYE/Freenove",
    -1,-1, 15,  4, 5,
    16,17,18,12, 10, 8, 9,11,
    6, 7,13);

/* 2 = Seeed XIAO ESP32S3 Sense */
static const pin_preset_t PRESET_XIAO = PRESET(
    "XIAO Sense",
    -1,-1, 10, 40,39,
    48,11,12,14, 16,18,17,15,
    38,47,13);

/* 3 = LilyGO T-Camera S3 (ESP32-S3-KAMERA-BOARD) */
static const pin_preset_t PRESET_LILYGO = PRESET(
    "LilyGO T-Camera S3",
    -1,39, 38,  5, 4,
    9,10,11,13, 21,48,47,14,
    8,18,12);

/* 4 = generic BOARD_ESP32S3_WROOM (esp32-camera example; pwdn=38) */
static const pin_preset_t PRESET_WROOM = PRESET(
    "BOARD_ESP32S3_WROOM",
    38,-1, 15,  4, 5,
    16,17,18,12, 10, 8, 9,11,
    6, 7,13);

static const pin_preset_t *const PRESETS[] = {
    NULL,               /* index 0 unused (1-based) */
    &PRESET_S3EYE,
    &PRESET_XIAO,
    &PRESET_LILYGO,
    &PRESET_WROOM,
};
#define PRESET_COUNT 4

/* ---- runtime state ---- */
static bool           s_inited = false;
/* Volatile state belongs only to this camera initialization. */
static bool s_square_pattern_active;
static uint8_t s_square_pattern_previous;
static int s_square_pattern_previous_status;
static unsigned       s_raw_buffer_count = 2;
static unsigned       s_jpeg_buffer_count = 0; /* 0: QVGA 4, larger 2 */
static bool           s_profile_dirty = false;
static unsigned       s_driver_buffer_count = 0;
static int            s_preset = 0;        /* 1-based, 0 = not yet detected */

int camera_vsync_pin(void)
{
    return s_preset >= 1 && s_preset <= PRESET_COUNT ? PRESETS[s_preset]->cfg.pin_vsync : -1;
}
static cam_res_t      s_res = CAM_RES_VGA;
static uint16_t       s_w = 0, s_h = 0;
static bool           s_is_gray = false;   /* native Y8 (true) vs JPEG (false) */
/* SCLK actually programmed by the active Y8 profile, in kHz. The row period
 * a rolling-shutter consumer needs is HTS/SCLK, and SCLK is only known where
 * the PLL is written, so record it there instead of re-deriving the whole
 * multiplier tree from registers. 0 = no Y8 profile programmed yet. */
static uint32_t       s_tracking_sclk_khz;
static uint32_t       s_tracking_pclk_mhz = 40;
static bool           s_sync_configured = false;
/* The most recently grabbed frame buffer. Kept here so camera_release() can
 * return it to esp32-camera after the TCP server has sent the JPEG bytes.
 * (Single-client, single-threaded server, so one slot is enough.) */
static camera_fb_t   *s_last_fb = NULL;
/* Small crash breadcrumbs retain buffer bounds without dumping image pixels. */
static COREDUMP_DRAM_ATTR volatile camera_fb_t s_recent_fb[2];
static COREDUMP_DRAM_ATTR volatile unsigned s_recent_fb_index;
static camera_tuning_t s_tuning = {
    .jpeg_quality = 20,
    .exposure_lines = -1,
    .gain = -1,
    .white_balance = 0,
    .brightness = 0,
    .contrast = 0,
    .saturation = 0,
    .sharpness = 0,
};
static bool s_tuning_loaded;
static bool s_orientation_override_valid;
static bool s_orientation_override_hmirror;
static bool s_orientation_override_vflip;

static bool tuning_valid(const camera_tuning_t *t)
{
    return t && t->jpeg_quality <= 63 &&
           (t->exposure_lines == -1 || (t->exposure_lines >= 1 && t->exposure_lines <= 1200)) &&
           (t->gain == -1 || (t->gain >= 0 && t->gain <= 64)) &&
           t->white_balance <= 5 &&
           t->brightness >= -3 && t->brightness <= 3 &&
           t->contrast >= -3 && t->contrast <= 3 &&
           t->saturation >= -4 && t->saturation <= 4 &&
           t->sharpness >= -3 && t->sharpness <= 3;
}

static void load_tuning(void)
{
    if (s_tuning_loaded) return;
    s_tuning_loaded = true;
    nvs_handle_t handle;
    if (nvs_open("cam_tune", NVS_READONLY, &handle) != ESP_OK) return;
    camera_tuning_t stored;
    size_t length = sizeof(stored);
    if (nvs_get_blob(handle, "profile", &stored, &length) == ESP_OK &&
        length == sizeof(stored) && tuning_valid(&stored)) {
        s_tuning = stored;
    }
    nvs_close(handle);
}

/* Cameras are mounted upside down to keep the USB cable accessible. Program
 * the sensor itself so JPEG and raw frames have the same orientation over
 * every transport (UVC and TCP) without host-side pixel transforms. */
static bool s_effective_orientation_valid = false;
/* Orientation defaults.
 *
 * These were both true, intended as a 180-degree rotation (which preserves
 * chirality and is therefore safe for triangulation). Measured on the decoded
 * pixels, the output was a PURE HORIZONTAL MIRROR instead: handwritten text
 * read backwards while remaining upright, and the scene stayed right-side up.
 * A mirror flips handedness, so a calibration solved from mirrored images
 * silently produces a reflected 3D solution.
 *
 * The vflip half was not reaching the output despite 0x3820 bit1 reading back
 * set, so the pair did not cancel. Ship the orientation that is provably
 * chirality-correct - no transform at all - rather than a rotation that only
 * half applies. ponytail: revisit if a module genuinely needs 180 degrees;
 * verify by reading text in a decoded frame, never by reading the registers
 * back. */
static bool s_effective_hmirror = false;
static bool s_effective_vflip = true;

static bool apply_mount_orientation_ordinary_owned(cam_res_t res)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGW(TAG, "cannot apply mount orientation: sensor unavailable");
        s_effective_orientation_valid = false;
        return false;
    }

    /* Mount orientation: hmirror=0, vflip=1.
     *
     * Established by capturing all four combinations and testing each against
     * the (0,0) reference for identity / H flip / V flip / 180 rotation, then
     * judging uprightness on gravity cues rather than on reading text - the
     * room is dim and letterforms proved unreliable. Objects rest on a glossy
     * shelf with their reflections BELOW them only in the vflip=1 column; at
     * vflip=0 the shelf sits at the top of the frame with items hanging from
     * it and reflections above, which gravity forbids.
     *
     * Measured transform of each setting relative to (0,0):
     *   h=0 v=0  identity      h=0 v=1  V flip
     *   h=1 v=0  180 rotation  h=1 v=1  180 rotation
     *
     * This is why the previous hmirror=1 was wrong: it applies a 180 rotation,
     * not the intended horizontal flip, so it traded a mirrored picture for an
     * upside-down one. vflip=1 alone is the pure vertical correction this
     * mount needs.
     *
     * A runtime `cam orient` does not survive a preview reconfigure - the
     * driver reapplies this default on every camera_start - so the default is
     * what matters. Verify by looking at decoded pixels, never by reading the
     * orientation registers back: 0x3820 bit 1 has read back set while the
     * output was unchanged. */
    bool hmirror = false;
    bool vflip = true;
    if (s_orientation_override_valid) {
        hmirror = s_orientation_override_hmirror;
        vflip = s_orientation_override_vflip;
    } else {
        intrinsic_t intrinsic;
        if (intrinsic_get(res, &intrinsic) && intrinsic.orientation_valid) {
            hmirror = intrinsic.hmirror != 0;
            vflip = intrinsic.vflip != 0;
        }
    }

    int mirror_result = s->set_hmirror ? s->set_hmirror(s, hmirror ? 1 : 0) : -1;
    int flip_result = s->set_vflip ? s->set_vflip(s, vflip ? 1 : 0) : -1;
    /* OV5640 night mode lengthens exposure by whole frame periods in dim
     * rooms. That made a nominal 30 FPS UVC stream physically deliver about
     * 5 FPS. Mocap uses illuminated markers and needs bounded frame timing,
     * so keep automatic exposure but forbid frame-rate reduction. */
    int night_result = s->set_aec2 ? s->set_aec2(s, 0) : 0;
    if (mirror_result != 0 || flip_result != 0) {
        s_effective_orientation_valid = false;
        ESP_LOGW(TAG, "sensor 180-degree rotation failed: hmirror=%d vflip=%d",
                 mirror_result, flip_result);
        return false;
    } else {
        s_effective_hmirror = hmirror;
        s_effective_vflip = vflip;
        s_effective_orientation_valid = true;
        ESP_LOGI(TAG, "sensor orientation: hmirror=%d vflip=%d%s; night_mode=%s",
                 hmirror, vflip,
                 hmirror && vflip ? " (180 degrees)" : "",
                 night_result == 0 ? "off" : "unsupported");
    }
    return true;
}

static bool apply_mount_orientation(cam_res_t res)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = apply_mount_orientation_ordinary_owned(res);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_orientation_override_ordinary_owned(bool hmirror, bool vflip)
{
    s_orientation_override_valid = true;
    s_orientation_override_hmirror = hmirror;
    s_orientation_override_vflip = vflip;
    return !s_inited || apply_mount_orientation(s_res);
}

bool camera_set_orientation_override(bool hmirror, bool vflip)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_orientation_override_ordinary_owned(hmirror, vflip);
    camera_sensor_mutation_end();
    return result;
}

static void camera_clear_orientation_override_ordinary_owned(void)
{
    s_orientation_override_valid = false;
    if (s_inited) apply_mount_orientation(s_res);
}

bool camera_clear_orientation_override(void)
{
    if (!camera_sensor_mutation_begin()) return false;
    camera_clear_orientation_override_ordinary_owned();
    camera_sensor_mutation_end();
    return true;
}

uint16_t camera_sensor_pid(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    return sensor ? sensor->id.PID : 0;
}

/* Last probed sensor PID; survives driver teardown (a lock-free read for
 * /health, which must not take the sensor lock). 0 = never probed. */
static volatile uint16_t s_detected_pid;
uint16_t camera_detected_pid(void) { return s_detected_pid; }

bool camera_get_effective_orientation(bool *hmirror, bool *vflip)
{
    if (!s_effective_orientation_valid) return false;
    if (hmirror) *hmirror = s_effective_hmirror;
    if (vflip) *vflip = s_effective_vflip;
    return true;
}

/* Map cam_res_t -> esp32-camera framesize + w/h. */
static framesize_t res_to_framesize(cam_res_t r, uint16_t *w, uint16_t *h)
{
    switch (r) {
        case CAM_RES_QVGA: *w = 320;  *h = 240;  return FRAMESIZE_QVGA;
        case CAM_RES_SXGA: *w = 1280; *h = 1024; return FRAMESIZE_SXGA;
        case CAM_RES_HD:   *w = 1280; *h = 720;  return FRAMESIZE_HD;
        case CAM_RES_FHD:  *w = 1920; *h = 1080; return FRAMESIZE_FHD;
        case CAM_RES_VGA:
        default:           *w = 640;  *h = 480;  return FRAMESIZE_VGA;
    }
}

/* Pixel format is part of esp32-camera's driver configuration, not merely a
 * sensor register. Changing only sensor->set_pixformat() leaves the driver's
 * framebuffer/parser configured for the old format; in practice that made
 * JPEG-sized payloads get mislabeled as grayscale. Reinitialize the driver
 * whenever the transport changes between JPEG and native sensor Y8. */
static bool reinit_camera(cam_res_t res, pixformat_t format)
{
    if (s_preset < 1 || s_preset > PRESET_COUNT) {
        ESP_LOGE(TAG, "cannot reinitialize camera without a detected preset");
        return false;
    }

    int preset = s_preset;
    if (s_last_fb) {
        esp_camera_fb_return(s_last_fb);
        s_last_fb = NULL;
    }
    if (s_inited) {
        camera_driver_deinit();
        s_inited = false;
        s_square_pattern_active = false;
    }

    camera_config_t cfg = PRESETS[preset]->cfg;
    uint16_t w, h;
    cfg.frame_size = res_to_framesize(res, &w, &h);
    cfg.pixel_format = format;
    /* Initialize the sensor at the largest calibration profile. cam_hal's
     * JPEG buffer floor below is sized for it; max-FPS mode later raises the
     * numeric quality to 50, which only makes frames smaller. */
    if (format == PIXFORMAT_JPEG) {
        cfg.jpeg_quality = 15;
        /* Continuous QVGA MJPEG is the throughput profile. Four buffers keep
         * the OV5640/DMA producer running while the USB or UDP consumer copies
         * and packetises the previous image. At larger resolutions the JPEG
         * buffers are substantially bigger, so retain the qualified two-buffer
         * allocation. Synchronized mode explicitly rebuilds this as one buffer
         * below so a cadence tick can never inherit a queue of old frames. */
        cfg.fb_count = s_jpeg_buffer_count ? s_jpeg_buffer_count : res == CAM_RES_QVGA ? 4 : 2;
    }
    if (format == PIXFORMAT_GRAYSCALE) {
        /* Two QVGA Y8 buffers let the camera driver acquire the next frame on
         * core 0 while core 1 scans the previous exact-pixel frame. Direct
         * PSRAM DMA remains deliberately disabled in sdkconfig because it
         * stalled these boards; qualify it separately before any Y8-only A/B.
         * Paced/FREX mode reinitializes this to one buffer below. */
        cfg.fb_count = s_raw_buffer_count;
        /* Grab-mode A/B (2026-09-16): LATEST and WHEN_EMPTY both hold the
         * serial loop at ~32 fps with fb_count 2 and 3 — the cadence pin is
         * inside the driver's frame-completion wait, not queue policy.
         * LATEST is kept because it returns the freshest frame (lowest
         * tracking latency). */
        cfg.grab_mode = CAMERA_GRAB_LATEST;
    }

    esp_err_t err = camera_driver_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera reinit format=%d failed: %s",
                 (int)format, esp_err_to_name(err));
        /* Only a missing sensor re-probes; OOM/busy keeps the known pinout. */
        if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) s_preset = 0;
        return false;
    }
    apply_mount_orientation(res);

    s_inited = true;
    s_preset = preset;
    s_profile_dirty = false;
    s_driver_buffer_count = cfg.fb_count;
    s_res = res;
    s_w = w;
    s_h = h;
    s_is_gray = (format == PIXFORMAT_GRAYSCALE);
    s_sync_configured = false;
    load_tuning();
    if (!camera_apply_tuning()) {
        ESP_LOGW(TAG, "one or more persisted sensor tuning controls failed");
    }
    ESP_LOGI(TAG, "camera reinitialized: %ux%u %s", w, h,
             s_is_gray ? "Y8" : "JPEG");
    return true;
}

/* Try one preset. Returns true and fills out_name on a successful probe. */
static bool try_preset(const pin_preset_t *p, char *sensor_name, size_t nname,
                       uint16_t *pid_out)
{
    /* esp_camera_init probes SCCB + ID registers; a wrong pinout either
     * returns an error or finds no sensor. */
    esp_err_t err = camera_driver_init(&p->cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "preset %s initialization failed: %s (0x%x), largest_dma=%u",
                 p->name, esp_err_to_name(err), (unsigned)err,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return false;
    }
    /* Read back the detected sensor info. */
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        camera_driver_deinit();
        return false;
    }
    apply_mount_orientation(CAM_RES_VGA);
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
    if (info && info->name) {
        strncpy(sensor_name, info->name, nname - 1);
        sensor_name[nname - 1] = '\0';
    } else {
        snprintf(sensor_name, nname, "unknown(0x%04x)", s->id.PID);
    }
    *pid_out = s->id.PID;
    /* NOTE: we leave the camera initialised so a subsequent camera_start() is
     * a fast framesize change instead of a full re-probe. */
    return true;
}

static cam_detect_result_t camera_detect_ordinary_owned(void)
{
    cam_detect_result_t res = { .ok = false, .preset = 0,
                                .preset_name = "none",
                                .sensor_name = "none", .pid = 0 };

    static char detected_name[32];   /* persists for the returned result pointer */

    /* Detection is an identity query once the driver is live. Re-probing used
     * to deinitialize the sensor underneath UVC, which immediately removed
     * the composite camera/CDC device from Windows. */
    if (s_inited && s_preset >= 1 && s_preset <= PRESET_COUNT) {
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor) {
            camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
            if (info && info->name) {
                strncpy(detected_name, info->name, sizeof(detected_name) - 1);
                detected_name[sizeof(detected_name) - 1] = '\0';
            } else {
                snprintf(detected_name, sizeof(detected_name),
                         "unknown(0x%04x)", sensor->id.PID);
            }
            res.ok = true;
            res.preset = s_preset;
            res.preset_name = PRESETS[s_preset]->name;
            res.sensor_name = detected_name;
            res.pid = sensor->id.PID;
            return res;
        }
    }

    /* If already inited, deinit first so the probe is clean. */
    if (s_inited) {
        if (s_last_fb) { esp_camera_fb_return(s_last_fb); s_last_fb = NULL; }
        camera_driver_deinit();
        s_inited = false;
        s_square_pattern_active = false;
        s_preset = 0;
    }

    int forced = CONFIG_FTMCS_CAM_PINSET;
    int first = (forced >= 1 && forced <= PRESET_COUNT) ? forced : 1;
    int last  = (forced >= 1 && forced <= PRESET_COUNT) ? forced : PRESET_COUNT;

    char name[32] = {0};
    for (int i = first; i <= last; i++) {
        const pin_preset_t *p = PRESETS[i];
        ESP_LOGI(TAG, "trying preset %d (%s)...", i, p->name);
        /* small delay so the log line flushes before a possible watchdog
         * during SCCB probing on a wrong pinout. */
        vTaskDelay(pdMS_TO_TICKS(50));
        uint16_t pid = 0;
        if (try_preset(p, name, sizeof(name), &pid)) {
            res.ok = true;
            res.preset = i;
            res.preset_name = p->name;
            strlcpy(detected_name, name, sizeof(detected_name));
            res.sensor_name = detected_name;
            res.pid = pid;
            s_detected_pid = pid;
            s_inited = true;
            s_preset = i;
            ESP_LOGI(TAG, "DETECTED: sensor=%s pid=0x%04x preset=%d (%s)",
                     name, pid, i, p->name);
            break;
        } else {
            ESP_LOGI(TAG, "preset %d (%s): no sensor", i, p->name);
        }
    }

    if (!res.ok) {
        ESP_LOGW(TAG, "no camera detected on any preset (tried %d..%d)", first, last);
    }
    return res;
}

cam_detect_result_t camera_detect(void)
{
    if (!camera_sensor_mutation_begin()) { return (cam_detect_result_t){0}; }
    cam_detect_result_t result = camera_detect_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_start_ordinary_owned(cam_res_t r)
{
    if (s_inited && !s_profile_dirty && !s_is_gray && r == s_res) return true;

    /* A format or geometry change must rebuild the driver configuration. */
    if (s_inited) return reinit_camera(r, PIXFORMAT_JPEG);

    if (s_preset >= 1 && s_preset <= PRESET_COUNT) return reinit_camera(r, PIXFORMAT_JPEG);
    /* Never detected: probe first (uses Kconfig forced preset or scans all). */
    cam_detect_result_t d = camera_detect();
    if (!d.ok) {
        ESP_LOGE(TAG, "camera_start: no sensor detected");
        return false;
    }
    bool ok = reinit_camera(r, PIXFORMAT_JPEG);
    if (ok) ESP_LOGI(TAG, "camera running: %s jpeg buffer-profile q%u",
                     d.sensor_name, s_tuning.jpeg_quality);
    return ok;
}

bool camera_start(cam_res_t r)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_start_ordinary_owned(r);
    camera_sensor_mutation_end();
    return result;
}

static void camera_stop_ordinary_owned(void)
{
    if (s_inited || s_sensor_teardown_unconfirmed || s_sensor_lifetime_ready) {
        if (s_last_fb) {
            esp_camera_fb_return(s_last_fb);
            s_last_fb = NULL;
        }
        camera_driver_deinit();
        s_inited = false;
        s_square_pattern_active = false;
        /* s_preset survives stop: the pinout is board wiring, so the next start
         * builds straight from it instead of paying a ~0.85 s probe init. */
        s_driver_buffer_count = 0;
        s_w = s_h = 0;
        s_is_gray = false;
        s_sync_configured = false;
        s_effective_orientation_valid = false;
        ESP_LOGI(TAG, "camera stopped");
    }
}

void camera_stop(void)
{
    if (!sensor_lifetime_take()) { return; }
    camera_stop_ordinary_owned();
    sensor_lifetime_give();
    
}

bool camera_running(void) { return s_inited; }

static int sensor_snapshot_read_byte(void *context, uint16_t address)
{
    sensor_t *sensor = context;
    return sensor->get_reg(sensor, address, 0xff);
}

static int64_t sensor_snapshot_now(void *context)
{
    (void)context;
    return esp_timer_get_time();
}

bool camera_print_sensor_registers(void)
{
    if (!sensor_lifetime_take()) return false;
    /* Do not use s_inited here: autonomous workers publish it outside this
     * lock. The lifetime state and actual getter are authoritative for safety. */
    sensor_t *sensor = s_sensor_lifetime_ready ? esp_camera_sensor_get() : NULL;
    if (!sensor || sensor->id.PID != OV5640_PID || !sensor->get_reg) {
        sensor_lifetime_give();
        return false;
    }
    const uint16_t pid = sensor->id.PID;
    sensor_register_snapshot_t snapshot;
    uint16_t failed_address = 0;
    sensor_snapshot_status_t status = sensor_snapshot_collect(sensor,
        sensor_snapshot_read_byte, sensor_snapshot_now, &snapshot, &failed_address);
    sensor_lifetime_give();
    /* No sensor pointers are dereferenced after release; printing owns only
     * copied PID and snapshot bytes. Slow console output cannot block teardown. */
    if (status != SENSOR_SNAPSHOT_OK) {
        printf("sensorregs failure=%u address=0x%04x\n", (unsigned)status, (unsigned)failed_address);
        return false;
    }
    printf("sensorregs pid=0x%04x count=%u before_us=%lld after_us=%lld nonatomic=1 writes=0\n",
        (unsigned)pid, (unsigned)SENSOR_SNAPSHOT_COUNT,
        (long long)snapshot.before_us, (long long)snapshot.after_us);
    for (size_t i = 0; i < SENSOR_SNAPSHOT_COUNT; ++i) {
        printf("sensorreg address=0x%04x value=0x%02x\n",
            (unsigned)sensor_snapshot_addresses[i], (unsigned)snapshot.values[i]);
    }
    return true;
}


void camera_get_framesize(uint16_t *w, uint16_t *h)
{
    if (w) *w = s_w;
    if (h) *h = s_h;
}

const uint8_t *camera_grab_jpeg(size_t *out_len, int64_t *out_capture_us)
{
    if (!s_inited) {
        if (out_len) *out_len = 0;
        if (out_capture_us) *out_capture_us = 0;
        return NULL;
    }
    /* Return any previously-held frame before grabbing the next, so we never
     * leak a frame buffer if the caller forgot to release. */
    if (s_last_fb) {
        esp_camera_fb_return(s_last_fb);
        s_last_fb = NULL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        if (out_len) *out_len = 0;
        if (out_capture_us) *out_capture_us = 0;
        return NULL;
    }
    s_recent_fb[s_recent_fb_index++ % 2] = *fb;
    /* We asked for JPEG; sanity-check the format. */
    if (fb->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(fb);
        if (out_len) *out_len = 0;
        if (out_capture_us) *out_capture_us = 0;
        return NULL;
    }
    s_last_fb = fb;
    if (out_len) *out_len = fb->len;
    /* Driver software capture-start time, assigned after processing its VSYNC
     * event. This is not a hardware-latched exposure or completion timestamp. */
    if (out_capture_us) {
        *out_capture_us = (int64_t)fb->timestamp.tv_sec * 1000000LL
                        + (int64_t)fb->timestamp.tv_usec;
    }
    /* Fire the strobe GPIO so a logic analyzer / future strobe sees the grab. */
    if (!frex_sync_active()) strobe_gpio_fire();
    /* Return the JPEG byte buffer, NOT the fb struct. camera_release() uses
     * the stored s_last_fb handle to return the buffer to esp32-camera. */
    return fb->buf;
}

void camera_release(const uint8_t *buf)
{
    /* buf is the JPEG byte buffer (fb->buf); the matching fb is in s_last_fb. */
    (void)buf;
    if (s_last_fb) {
        esp_camera_fb_return(s_last_fb);
        s_last_fb = NULL;
    }
}

static bool camera_start_grayscale_ordinary_owned(cam_res_t res)
{
    if (s_inited && !s_profile_dirty && s_is_gray && res == s_res) return true;
    if (!s_inited && (s_preset < 1 || s_preset > PRESET_COUNT)) {
        cam_detect_result_t detected = camera_detect();
        if (!detected.ok) return false;
    }
    return reinit_camera(res, PIXFORMAT_GRAYSCALE);
}

bool camera_start_grayscale(cam_res_t res)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_start_grayscale_ordinary_owned(res);
    camera_sensor_mutation_end();
    return result;
}

static const uint8_t *camera_grab_gray_impl(size_t *out_len, int *out_y_stride,
                                            int64_t *out_capture_us,
                                            bool emit_strobe, uint32_t timeout_ms)
{
    if (!s_inited || !s_is_gray) {
        if (out_len) *out_len = 0;
        if (out_y_stride) *out_y_stride = 0;
        if (out_capture_us) *out_capture_us = 0;
        return NULL;
    }
    if (s_last_fb) { esp_camera_fb_return(s_last_fb); s_last_fb = NULL; }
    /* FREX: the driver arms only on a trigger's image VSYNC, so a grab that
     * waits for a missed frame must return and let the caller trigger again,
     * not block the trigger loop for the default 4 s.
     * ponytail: 100 ms = >3 slots at 35 fps; derive from the slot if paced < 12 fps. */
    if (!timeout_ms) timeout_ms = frex_sync_active() ? 100 : 4000;
    camera_fb_t *fb = esp_camera_fb_get_timeout(timeout_ms);
    if (!fb) {
        if (out_len) *out_len = 0;
        if (out_y_stride) *out_y_stride = 0;
        if (out_capture_us) *out_capture_us = 0;
        return NULL;
    }
    s_recent_fb[s_recent_fb_index++ % 2] = *fb;
    size_t expected = (size_t)s_w * (size_t)s_h;
    if (fb->format != PIXFORMAT_GRAYSCALE || fb->len != expected) {
        ESP_LOGE(TAG, "invalid grayscale frame: format=%d len=%u expected=%u",
                 (int)fb->format, (unsigned)fb->len, (unsigned)expected);
        esp_camera_fb_return(fb);
        if (out_len) *out_len = 0;
        if (out_y_stride) *out_y_stride = 0;
        if (out_capture_us) *out_capture_us = 0;
        return NULL;
    }
    s_last_fb = fb;
    if (out_len) *out_len = fb->len;
    /* OV5640 native grayscale emits one luminance byte per pixel. */
    if (out_y_stride) *out_y_stride = 1;
    if (out_capture_us) {
        *out_capture_us = (int64_t)fb->timestamp.tv_sec * 1000000LL
                        + (int64_t)fb->timestamp.tv_usec;
    }
    /* Fire the strobe so the LA sees the grab instant. */
    if (emit_strobe && !frex_sync_active()) strobe_gpio_fire();
    return fb->buf;
}

const uint8_t *camera_grab_gray(size_t *out_len, int *out_y_stride,
                                int64_t *out_capture_us)
{
    return camera_grab_gray_impl(out_len, out_y_stride, out_capture_us, true, 0);
}

const uint8_t *camera_grab_gray_timeout(size_t *out_len, int *out_y_stride,
                                        int64_t *out_capture_us, uint32_t timeout_ms)
{
    return camera_grab_gray_impl(out_len, out_y_stride, out_capture_us, true, timeout_ms);
}

const uint8_t *camera_grab_gray_quiet(size_t *out_len, int *out_y_stride,
                                      int64_t *out_capture_us)
{
    return camera_grab_gray_impl(out_len, out_y_stride, out_capture_us, false, 0);
}

bool camera_is_grayscale(void) { return s_is_gray; }

static bool camera_prime_preview_ordinary_owned(void)
{
    if (!s_inited || s_last_fb) return false;
    const int64_t configured_us = esp_timer_get_time();
    const int64_t deadline_us = configured_us + 8000000;
    int64_t last_us = configured_us;
    unsigned fresh = 0;
    while (esp_timer_get_time() < deadline_us) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) return false;
        int64_t timestamp = (int64_t)fb->timestamp.tv_sec * 1000000LL + fb->timestamp.tv_usec;
        esp_camera_fb_return(fb);
        if (timestamp > last_us) {
            last_us = timestamp;
            if (++fresh == 2) return true;
        }
    }
    ESP_LOGE(TAG, "preview did not produce fresh post-configuration frames");
    return false;
}

bool camera_prime_preview(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_prime_preview_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_config_buffering_ordinary_owned(bool synchronized)
{
    if (s_inited && s_sync_configured == synchronized) return true;
    if (!s_inited && (s_preset < 1 || s_preset > PRESET_COUNT)) return false;
    /* esp32-camera doesn't expose runtime grab_mode/fb_count changes directly,
     * so we deinit + reinit with the sync-optimized config. Native-Y keeps a
     * single FIFO buffer. JPEG uses two latest-frame buffers: at 100+ Hz the
     * previous frame can still be queued when the next FREX edge is issued,
     * and consuming it first costs an entire additional sensor period. */
    int preset = s_preset;
    cam_res_t res = s_res;
    bool grayscale = s_is_gray;
    if (preset < 1 || preset > PRESET_COUNT) return false;

    if (s_last_fb) {
        esp_camera_fb_return(s_last_fb);
        s_last_fb = NULL;
    }
    if (s_inited) camera_driver_deinit();
    s_inited = false;
    s_square_pattern_active = false;

    /* Clone the preset config but override fb_count + grab_mode. */
    camera_config_t cfg = PRESETS[preset]->cfg;
    uint16_t w, h;
    cfg.frame_size = res_to_framesize(res, &w, &h);
    /* Two Y8 buffers in synchronized mode let the sensor fill frame N+1 while
     * the detector still scans N. GRAB_WHEN_EMPTY keeps a FREX trigger bound
     * to one exposure - the driver only starts a new frame into a free buffer,
     * so no trigger is ever serviced by a stale image. Under test; s_raw_sync_buffers
     * is the A/B knob (`cam syncbufs`). */
    cfg.fb_count = synchronized
        ? (grayscale ? s_raw_sync_buffers : 2)
        : (grayscale ? s_raw_buffer_count : s_jpeg_buffer_count ? s_jpeg_buffer_count : res == CAM_RES_QVGA ? 4 : 2);
    /* GRAB_WHEN_EMPTY is what binds a FREX trigger to one exposure: the driver
     * only starts a new frame into a FREE buffer, so no trigger is ever
     * serviced by a stale image. That reasoning is not grayscale-specific, but
     * the mode was only applied to grayscale, leaving synchronized JPEG on
     * GRAB_LATEST.
     *
     * Measured at VGA with fb_count=2 and GRAB_LATEST: consecutive frames at
     * identical settings alternated between two DISCRETE states - 14766 bytes
     * reading a uniform luma 253 (distinct=1) and 15216 bytes reading luma
     * 190.4 (distinct=21). The byte sizes were exactly repeatable, which is a
     * buffer alternation, not sensor noise or corruption; every JPEG decoded
     * cleanly with a valid EOI. 720p and QVGA were stable in the same test.
     *
     * Bind the trigger for every synchronized pixel format. */
    cfg.grab_mode = synchronized
        ? CAMERA_GRAB_WHEN_EMPTY : CAMERA_GRAB_LATEST;
    cfg.pixel_format = grayscale ? PIXFORMAT_GRAYSCALE : PIXFORMAT_JPEG;
    if (!grayscale) cfg.jpeg_quality = 15;

    esp_err_t err = camera_driver_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sync-mode reinit failed: %s", esp_err_to_name(err));
        /* Only a missing sensor re-probes; OOM/busy keeps the known pinout. */
        if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) s_preset = 0;
        return false;
    }
    apply_mount_orientation(res);
    s_inited = true;
    s_is_gray = grayscale;
    s_preset = preset;
    s_res = res;
    s_w = w; s_h = h;
    s_sync_configured = synchronized;
    s_driver_buffer_count = cfg.fb_count;
    s_profile_dirty = false;
    load_tuning();
    if (!camera_apply_tuning()) {
        ESP_LOGW(TAG, "one or more persisted sensor tuning controls failed");
    }
    ESP_LOGI(TAG, "%s mode: %ux%u %s fb_count=%d %s",
             synchronized ? "sync" : "continuous", w, h,
             grayscale ? "gray" : "jpeg", cfg.fb_count,
             cfg.grab_mode == CAMERA_GRAB_WHEN_EMPTY ? "GRAB_WHEN_EMPTY" : "GRAB_LATEST");
    return true;
}

/* Native Y8 in synchronized buffering with ONE driver init. start_grayscale
 * followed by config_sync_mode built the driver twice (plus a probe init
 * after every stop): ~2.5 s of each tracking/preview start. */
bool camera_start_grayscale_sync(cam_res_t res)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool ok = true;
    if (!(s_inited && !s_profile_dirty && s_is_gray && res == s_res && s_sync_configured)) {
        if (s_preset < 1 || s_preset > PRESET_COUNT) ok = camera_detect_ordinary_owned().ok;
        if (ok) {
            s_res = res;
            s_is_gray = true;
            s_sync_configured = false; /* forces the rebuild below */
            ok = camera_config_buffering_ordinary_owned(true);
        }
    }
    camera_sensor_mutation_end();
    return ok;
}

static bool camera_config_buffering(bool synchronized)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_config_buffering_ordinary_owned(synchronized);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_config_sync_mode_ordinary_owned(void)
{
    return camera_config_buffering(true);
}

bool camera_config_sync_mode(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_config_sync_mode_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_config_continuous_mode_ordinary_owned(void)
{
    return camera_config_buffering(false);
}

bool camera_config_continuous_mode(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_config_continuous_mode_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static void camera_set_exposure_ordinary_owned(int level)
{
    if (!s_inited) return;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    if (level < 0) {
        /* restore auto exposure */
        if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
        ESP_LOGI(TAG, "exposure: auto");
    } else {
        if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 0); /* disable AE */
        if (s->set_ae_level)     s->set_ae_level(s, level);   /* signed, sensor units */
        ESP_LOGI(TAG, "exposure: manual level=%d", level);
    }
}

bool camera_set_exposure(int level)
{
    if (!camera_sensor_mutation_begin()) return false;
    camera_set_exposure_ordinary_owned(level);
    camera_sensor_mutation_end();
    return true;
}

/* The driver's set_aec_value() writes its argument straight into
 * 0x3500[3:0]:0x3501[7:0]:0x3502[7:4], which the OV5640 reads as exposure in
 * SIXTEENTHS of a row - the low four bits are fractional. Passing a row count
 * therefore exposes 16x too briefly: asking for 100 rows read back as 6.
 *
 * It also clamps against 0x380e (VTS) in those same sixteenth units, so any
 * request above VTS/16 rows was silently truncated - at VTS 744 that capped
 * every mode at 46 rows however much was asked for. That is why luma sat flat
 * at 5.0 across a 9x exposure sweep in synchronized mode.
 *
 * Convert rows to sixteenths here, and clamp in rows against VTS so the limit
 * means what it says. */
static int camera_set_exposure_rows(sensor_t *s, int rows)
{
    if (!s || !s->set_aec_value) return -1;
    int vts = 0;
    if (s->get_reg) {
        int hi = s->get_reg(s, 0x380e, 0xff);
        int lo = s->get_reg(s, 0x380f, 0xff);
        if (hi >= 0 && lo >= 0) vts = (hi << 8) | lo;
    }
    /* Leave a two-row margin: exposure must end before the frame does. */
    if (vts > 2 && rows > vts - 2) rows = vts - 2;
    if (rows < 1) rows = 1;
    /* Write 0x3500-02 directly rather than via set_aec_value(). That helper
     * clamps with `max_val = read_reg16(0x380e)` - VTS, a ROW count - against
     * a value expressed in SIXTEENTHS of a row, a limit 16x tighter than
     * intended. At VTS 744 it truncated every request above 46.5 rows, which
     * the hardware confirmed by reading back exactly 0x2e8 = 744 sixteenths.
     * The clamp in rows above is the correct one. */
    if (!s->set_reg) return s->set_aec_value(s, rows * 16);
    /* The 20-bit field is 0x3500[3:0]:0x3501[7:0]:0x3502[7:4], i.e. rows with
     * four FRACTIONAL bits, so the stored integer is rows * 16 split as
     * [19:16][15:8][7:0]. An earlier version of this function reused the
     * driver's shift pattern (>>12, >>4, <<4), which spreads the value one
     * nibble too far left and stores rows * 256: `cam tune` reported
     * exposure_rows=1600 for a 100-row request, 4800 for 300. The low nibble
     * of 0x3502 is the fractional part and stays zero for whole rows. */
    int units = rows * 16;
    int rc = s->set_reg(s, 0x3500, 0xff, (units >> 16) & 0x0f);
    rc |= s->set_reg(s, 0x3501, 0xff, (units >> 8) & 0xff);
    rc |= s->set_reg(s, 0x3502, 0xff, units & 0xff);
    return rc;
}

static bool camera_set_tracking_exposure_ordinary_owned(uint16_t lines)
{
    if (!s_inited) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_exposure_ctrl || !s->set_aec_value) return false;
    int ctrl = s->set_exposure_ctrl(s, 0);
    int value = camera_set_exposure_rows(s, lines);
    /* Record it in the tuning state as well as the live registers. Every
     * sensor rebuild runs camera_apply_tuning(), which re-enables AUTO
     * exposure when exposure_lines < 0 - so a live-only write survived until
     * the next rebuild and then silently reverted. Observed as brightness
     * oscillating 6 <-> 251 on a supposedly fixed exposure while AEC hunted. */
    s_tuning.exposure_lines = (int)lines;
    ESP_LOGI(TAG, "tracking exposure: fixed %u lines (%s)", lines,
             ctrl == 0 && value == 0 ? "ok" : "failed");
    return ctrl == 0 && value == 0;
}

bool camera_set_tracking_exposure(uint16_t lines)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_tracking_exposure_ordinary_owned(lines);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_tracking_gain_ordinary_owned(uint8_t gain)
{
    if (!s_inited) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_gain_ctrl || !s->set_agc_gain) return false;
    int ctrl = s->set_gain_ctrl(s, 0);
    int value = s->set_agc_gain(s, gain);
    /* Persist for the same reason as exposure: a rebuild re-enables auto gain
     * when s_tuning.gain < 0, undoing a live-only write. */
    s_tuning.gain = (int)gain;
    ESP_LOGI(TAG, "tracking gain: fixed %u (%s)", gain,
             ctrl == 0 && value == 0 ? "ok" : "failed");
    return ctrl == 0 && value == 0;
}

/* Live AGC gain, the companion to camera_set_tracking_exposure().
 *
 * Exposure alone cannot expose for a bright subject when gain stays pinned
 * high for IR markers: the sensor saturates regardless. Gain was previously
 * reachable only through camera_apply_tuning(), whose `cam tune` entry point
 * routes exposure to set_ae_level and is therefore unusable for real values. */
bool camera_set_tracking_gain(uint8_t gain)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_tracking_gain_ordinary_owned(gain);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_jpeg_quality_ordinary_owned(uint8_t quality)
{
    if (!s_inited || s_is_gray || quality > 63) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_quality) return false;
    int ret = s->set_quality(s, quality);
    /* Persist alongside the live write: camera_apply_tuning() re-applies
     * s_tuning.jpeg_quality on every sensor rebuild, so a live-only change
     * silently reverts the next time the sync loop rebuilds the sensor. */
    s_tuning.jpeg_quality = quality;
    ESP_LOGI(TAG, "JPEG quality=%u (%s)", quality, ret == 0 ? "ok" : "failed");
    return ret == 0;
}

bool camera_set_jpeg_quality(uint8_t quality)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_jpeg_quality_ordinary_owned(quality);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_apply_tuning_ordinary_owned(void)
{
    if (!s_inited) return true;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    int ret = 0;
    if (!s_is_gray && s->set_quality) ret |= s->set_quality(s, s_tuning.jpeg_quality);
    if (s_tuning.exposure_lines < 0) {
        if (s->set_exposure_ctrl) ret |= s->set_exposure_ctrl(s, 1);
    } else {
        if (!s->set_exposure_ctrl || !s->set_aec_value) return false;
        /* OV5640 integrates in row periods, so an exposure at or above the
         * frame's total row count (VTS) cannot complete inside one frame: the
         * sensor then returns empty buffers and capture goes black and stays
         * black. Measured directly - exposure 200 gave frame peak 134, while
         * 600 and 1000 against the tracking VTS of 1000 rows both gave peak 4.
         * Clamp to the live VTS so a high slider value saturates brightness
         * instead of killing the stream. */
        int exposure = s_tuning.exposure_lines;
        if (s->get_reg) {
            int vts = s->get_reg(s, 0x380e, 0xffff);
            if (vts > 4) {
                int ceiling = vts - 4; /* keep the sensor's blanking margin */
                if (exposure > ceiling) {
                    ESP_LOGW(TAG,
                             "exposure %d >= VTS %d; clamping to %d rows",
                             exposure, vts, ceiling);
                    exposure = ceiling;
                }
            }
        }
        ret |= s->set_exposure_ctrl(s, 0);
        ret |= camera_set_exposure_rows(s, exposure);
    }
    if (s_tuning.gain < 0) {
        if (s->set_gain_ctrl) ret |= s->set_gain_ctrl(s, 1);
    } else {
        if (!s->set_gain_ctrl || !s->set_agc_gain) return false;
        ret |= s->set_gain_ctrl(s, 0);
        ret |= s->set_agc_gain(s, s_tuning.gain);
    }
    if (s_tuning.white_balance == 5) {
        if (s->set_whitebal) ret |= s->set_whitebal(s, 0);
        if (s->set_awb_gain) ret |= s->set_awb_gain(s, 0);
    } else {
        if (s->set_whitebal) ret |= s->set_whitebal(s, 1);
        if (s->set_awb_gain) ret |= s->set_awb_gain(s, 1);
        if (s->set_wb_mode) ret |= s->set_wb_mode(s, s_tuning.white_balance);
    }
    if (s->set_brightness) ret |= s->set_brightness(s, s_tuning.brightness);
    if (s->set_contrast) ret |= s->set_contrast(s, s_tuning.contrast);
    if (s->set_saturation) ret |= s->set_saturation(s, s_tuning.saturation);
    if (s->set_sharpness) ret |= s->set_sharpness(s, s_tuning.sharpness);
    /* Under FREX the rolling-shutter exposure above is ignored; the FREX
     * registers set the integration time. */
    if (!frex_sync_reapply_exposure_ordinary_owned()) ret |= 1;
    ESP_LOGI(TAG,
             "tuning: q=%u exp=%d gain=%d wb=%u bri=%d con=%d sat=%d sharp=%d (%s)",
             s_tuning.jpeg_quality, s_tuning.exposure_lines, s_tuning.gain,
             s_tuning.white_balance, s_tuning.brightness, s_tuning.contrast,
             s_tuning.saturation, s_tuning.sharpness, ret == 0 ? "ok" : "failed");
    return ret == 0;
}

bool camera_apply_tuning(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_apply_tuning_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_tuning_ordinary_owned(const camera_tuning_t *tuning, bool persist)
{
    if (!tuning_valid(tuning)) return false;
    s_tuning = *tuning;
    s_tuning_loaded = true;
    if (persist) {
        nvs_handle_t handle;
        if (nvs_open("cam_tune", NVS_READWRITE, &handle) != ESP_OK) return false;
        esp_err_t err = nvs_set_blob(handle, "profile", &s_tuning, sizeof(s_tuning));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
        if (err != ESP_OK) return false;
    }
    return camera_apply_tuning();
}

bool camera_set_tuning(const camera_tuning_t *tuning, bool persist)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_tuning_ordinary_owned(tuning, persist);
    camera_sensor_mutation_end();
    return result;
}

void camera_get_tuning(camera_tuning_t *out)
{
    if (!out) return;
    load_tuning();
    *out = s_tuning;
}

static bool s_perf_profile = true;
static bool camera_set_jpeg_buffer_count_ordinary_owned(unsigned count)
{
    if (count != 0 && (count < 2 || count > 4)) return false;
    s_profile_dirty = true;
    s_jpeg_buffer_count = count;
    return true;
}

bool camera_set_jpeg_buffer_count(unsigned count)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_jpeg_buffer_count_ordinary_owned(count);
    camera_sensor_mutation_end();
    return result;
}
static unsigned s_qvga_pclk_div = 8; /* 100/25 MHz; 0 retains 87.5/35 control. */
static bool camera_set_qvga_pclk_div_ordinary_owned(unsigned divisor)
{
    if (divisor != 0 && (divisor < 5 || divisor > 8)) return false;
    s_profile_dirty = true;
    s_qvga_pclk_div = divisor;
    return true;
}

bool camera_set_qvga_pclk_div(unsigned divisor)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_qvga_pclk_div_ordinary_owned(divisor);
    camera_sensor_mutation_end();
    return result;
}
static void camera_set_raw_direct_dma_ordinary_owned(bool enabled)
{
    s_profile_dirty = true;
    s_raw_direct_dma = enabled;
}

bool camera_set_copy_prio(unsigned prio)
{
    if ((prio && (prio < 20 || prio >= configMAX_PRIORITIES)) || !camera_sensor_mutation_begin()) return false;
    s_copy_prio = prio;
    s_profile_dirty = true;
    camera_sensor_mutation_end();
    return true;
}
bool camera_set_raw_direct_dma(bool enabled)
{
    if (!camera_sensor_mutation_begin()) return false;
    camera_set_raw_direct_dma_ordinary_owned(enabled);
    camera_sensor_mutation_end();
    return true;
}
bool camera_set_raw_eof_trace(bool enabled)
{
    if (strobe_gpio_get_mode() != STROBE_OFF || s_inited) return false;
    return ftmcs_camera_eof_trace_arm(enabled);
}

bool camera_print_raw_eof_trace(void)
{
    if (strobe_gpio_get_mode() != STROBE_OFF || s_inited) return false;
    ftmcs_eof_trace_meta_t m;
    ftmcs_camera_eof_trace_meta(&m);
    printf("rawtrace order=software_publication snapshot_nonatomic=1 cycles=per_core_uint32 unknown=4294967295 event_unknown=255\n");
    printf("rawtrace count=%u capacity=%u overwritten=%u frozen=%u armed=%u epoch=%u eof_serial=%u serial=%u expected=%u chunk=%u node=%u ring=%u record_bytes=%u\n",
           (unsigned)m.count, (unsigned)FTMCS_EOF_TRACE_CAPACITY, (unsigned)m.overwritten,
           (unsigned)m.frozen, (unsigned)m.armed, (unsigned)m.epoch, (unsigned)m.eof_serial,
           (unsigned)m.serial, (unsigned)m.expected_bytes, (unsigned)m.chunk_bytes,
           (unsigned)m.node_bytes, (unsigned)m.ring_bytes, (unsigned)sizeof(ftmcs_eof_trace_record_t));
    for (uint32_t i = 0; i < m.count; ++i) {
        ftmcs_eof_trace_record_t v;
        if (!ftmcs_camera_eof_trace_read(i, &v)) return false;
        printf("rawtrace row=%u serial=%u epoch=%u eof_serial=%u flags=%u cycles_begin=%u cycles_end=%u lcd_raw=%08x lcd_status=%08x dma_raw=%08x dma_status=%08x dma_enabled=%08x eof_desc=%08x current_desc=%08x previous_desc=%08x link=%08x chunks=%u bytes=%u kind=%u event=%u core=%u state=%u\n",
               (unsigned)i, (unsigned)v.serial, (unsigned)v.epoch, (unsigned)v.eof_serial,
               (unsigned)v.flags, (unsigned)v.cycles_begin, (unsigned)v.cycles_end,
               (unsigned)v.lcd_raw, (unsigned)v.lcd_status, (unsigned)v.dma_raw,
               (unsigned)v.dma_status, (unsigned)v.dma_enabled, (unsigned)v.eof_descriptor,
               (unsigned)v.current_descriptor, (unsigned)v.previous_descriptor, (unsigned)v.link,
               (unsigned)v.copied_chunks, (unsigned)v.copied_bytes, (unsigned)v.kind,
               (unsigned)v.event, (unsigned)v.core, (unsigned)v.state);
    }
    return true;
}

static bool camera_set_raw_dma_chunk_ordinary_owned(unsigned bytes)
{
    if (strobe_gpio_get_mode() != STROBE_OFF ||
        (bytes != 0 && bytes != 7680 && bytes != 3840)) return false;
    s_profile_dirty = true;
    s_raw_dma_chunk_requested = bytes;
    return true;
}

bool camera_set_raw_dma_chunk(unsigned bytes)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_raw_dma_chunk_ordinary_owned(bytes);
    camera_sensor_mutation_end();
    return result;
}
unsigned camera_get_raw_dma_chunk(void)
{
    return s_raw_dma_chunk_requested;
}
static bool camera_set_raw_dma_ring_ordinary_owned(unsigned kib)
{
    if (kib != 0 && kib != 32 && kib != 48 && kib != 64) return false;
    s_profile_dirty = true;
    s_raw_dma_ring_kib = kib;
    return true;
}

bool camera_set_raw_dma_ring(unsigned kib)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_raw_dma_ring_ordinary_owned(kib);
    camera_sensor_mutation_end();
    return result;
}
static bool camera_set_raw_buffer_count_ordinary_owned(unsigned count)
{
    /* This only stages the next reinitialization; mode OFF may retain an
     * initialized sensor after tracking stops. The command enforces OFF. */
    if (count < 2 || count > 4) return false;
    s_profile_dirty = true;
    s_raw_buffer_count = count;
    return true;
}

bool camera_set_raw_buffer_count(unsigned count)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_raw_buffer_count_ordinary_owned(count);
    camera_sensor_mutation_end();
    return result;
}
bool camera_set_raw_sync_buffers(unsigned count)
{
    if (count < 1 || count > 3) return false;
    if (!camera_sensor_mutation_begin()) { return false; }
    s_profile_dirty = true;
    s_raw_sync_buffers = count;
    camera_sensor_mutation_end();
    return true;
}
static unsigned s_tracking_vco_mhz = 0; /* Automatic per-resolution selection. */
static bool camera_set_tracking_vco_ordinary_owned(unsigned mhz)
{
    if (mhz != 0 && (mhz < 360 || mhz > 640 || mhz % CONFIG_FTMCS_CAM_XCLK_MHZ)) return false;
    s_profile_dirty = true;
    s_tracking_vco_mhz = mhz;
    return true;
}

bool camera_set_tracking_vco(unsigned mhz)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_tracking_vco_ordinary_owned(mhz);
    camera_sensor_mutation_end();
    return result;
}
static void camera_set_perf_profile_ordinary_owned(bool enabled)
{
    /* Rebuild on the next start so a retained driver or diagnostic trial
     * cannot inherit the previous PLL/HTS or an old framebuffer allocation. */
    s_profile_dirty = true;
    s_perf_profile = enabled;
}

bool camera_set_perf_profile(bool enabled)
{
    if (!camera_sensor_mutation_begin()) return false;
    camera_set_perf_profile_ordinary_owned(enabled);
    camera_sensor_mutation_end();
    return true;
}
bool camera_get_perf_profile(void) { return s_perf_profile; }

static bool camera_enable_fast_tracking_profile_owned(void)
{
    if (!s_inited) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->id.PID != 0x5640 || !s->set_res_raw || !s->set_reg) {
        return false;
    }

    if (s_is_gray && s_res != CAM_RES_QVGA) {
        /* Preserve stock crop/binning/scaling. The stock non-JPEG PLL is
         * the larger-mode bottleneck. 560 MHz VCO / sys-div 2 / 8-bit roots
         * gives matching 35 MHz SCLK and PCLK at XCLK=20 MHz; equal clocks
         * leave enough output clocks for every active pixel on each line.
         * Unlike the QVGA 87.5/35 profile this does not oversubscribe DVP
         * when output width is 1280 or 1920. Validate before using frames. */
        if (!s->set_pll || !s->get_reg) return false;
        /* VGA measured 2026-09-17 on node 0, same scene, `cam rawclock N`:
         *   VCO 400 (old default) PCLK 20 MHz  grab 44.4 ms  12.0 fps
         *   VCO 480               PCLK 24 MHz  grab 37.2 ms  15.0 fps
         *   VCO 560               PCLK 28 MHz  grab 31.9 ms  14.2 fps
         *   VCO 640               PCLK 32 MHz  grab 28.2 ms  14.9 fps
         * Grab time is the dominant stage and scales with PCLK. 640 was
         * verified for content, not just rate: capture_fail=0, truncated=0,
         * detector_overflow=0 and 1.00 markers/frame, so it is not the kind of
         * "faster" that was really comb corruption. */
        const unsigned selected_vco = s_tracking_vco_mhz ? s_tracking_vco_mhz :
                                      s_res == CAM_RES_VGA ? 640U : 560U;
        const uint32_t multiplier = (s_perf_profile ? selected_vco : 560U) / CONFIG_FTMCS_CAM_XCLK_MHZ;
        /* HD/FHD need a complete wide output line. Keep SCLK=PCLK for those
         * modes; their previous 2:1 profiles dropped frames or stalled. VGA
         * and SXGA retain the independently tested faster sensor clock. */
        const bool fast_sensor = s_perf_profile && s_res != CAM_RES_HD && s_res != CAM_RES_FHD;
        const int sys_div = fast_sensor ? 1 : 2;
        const int pclk_div = fast_sensor ? 4 : 2;
        int ret = s->set_pll(s, 0, (int)multiplier, sys_div, 0, 1, 1, 1, pclk_div);
        ret |= s->set_reg(s, 0x3034, 0x0f, 8);
        if (ret || (s->get_reg(s, 0x3034, 0xff) & 0x0f) != 8 ||
            s->get_reg(s, 0x3035, 0xff) != (sys_div == 1 ? 0x11 : 0x21) ||
            s->get_reg(s, 0x3036, 0xff) != (int)multiplier ||
            s->get_reg(s, 0x3037, 0xff) != 0x01 ||
            s->get_reg(s, 0x3108, 0xff) != 0x16 ||
            s->get_reg(s, 0x3824, 0xff) != pclk_div) {
            ESP_LOGE(TAG, "large Y8 PLL verification failed");
            return false;
        }
        ESP_LOGI(TAG, "native Y8 %ux%u: SCLK=%lu PCLK=%lu kHz, full-field geometry",
                 s_w, s_h,
                 (unsigned long)(CONFIG_FTMCS_CAM_XCLK_MHZ * multiplier * 1000U / (8U * sys_div)),
                 (unsigned long)(CONFIG_FTMCS_CAM_XCLK_MHZ * multiplier * 1000U / (4U * sys_div * pclk_div)));
        /* Row period = HTS/SCLK, so the rolling-shutter timestamp depends on
         * the clock this branch just programmed. */
        s_tracking_sclk_khz = CONFIG_FTMCS_CAM_XCLK_MHZ * multiplier * 1000U / (8U * sys_div);
        /* VTS trim rejected 2026-09-17: writing 0x380e/0x380f (700/550/450
         * rows) after set_pll stopped DVP output entirely - camera init never
         * completed and `cam tune` reported no live registers. Shortening the
         * frame this way also fights the driver's own timing programming and
         * the AEC row clamp. Raise PCLK instead; that is what VCO 640 does. */
        return true;
    }
    if (s_res != CAM_RES_QVGA) return false;

    if (s_is_gray) {
        /* Zephyr's maintained OV5640 DVP QVGA geometry: full 4:3 sensor field,
         * 2:1 sensor binning, then ISP scaling to exact 320x240 output. The
         * 1556x1000 timing envelope is substantially smaller than esp-camera's
         * stock 2060x984 envelope while retaining the same field of view.
         * Run at the ESP32-S3's documented 40 MHz DVP ceiling. */
        int ret = camera_set_tracking_subsample(2) ? 0 : -1;
        uint32_t actual_mhz = 0;
        if (ret != 0 ||
            !camera_set_tracking_pclk(s_tracking_pclk_mhz, &actual_mhz)) {
            ESP_LOGW(TAG, "OV5640 native-Y8 QVGA profile failed: %d", ret);
            return false;
        }
        ret |= s->set_reg(s, 0x3a02, 0xffff, 0x03e8);
        ret |= s->set_reg(s, 0x3a14, 0xffff, 0x03e8);
        if (ret != 0 || !apply_mount_orientation(s_res)) return false;
        ESP_LOGI(TAG,
                 "OV5640 native-Y8 QVGA DVP profile active (PCLK=%lu MHz)",
                 (unsigned long)actual_mhz);
        return true;
    }

    /* High-rate OV5640 JPEG QVGA profile. Unlike the previous experimental
     * reduced window, this keeps the documented full-array crop, 2:1 sensor
     * binning and ISP scaling, so field of view and image geometry remain
     * unchanged. Do not overwrite 0x3820/0x3821 here: those contain the
     * persisted hmirror/vflip orientation bits.
     *
     * This profile also pins VTS to 984 rows (0x3a02/0x3a14 = 0x03d8). That is
     * only reached from configure_udp_sensor()'s high-rate QVGA path, which is
     * now off by default (see sync_capture.c) because it renders black on these
     * modules. */
    int ret = s->set_res_raw(s,
                             0, 4, 2623, 1947,
                             16, 6,
                             1896, 984,
                             320, 240,
                             true, true);
    ret |= s->set_reg(s, 0x3035, 0xff, 0x14);
    /* 0x60 is the highest sustained-stable clock measured on the installed
     * modules (~51.5 centroid frames/s). 0x68 and 0x70 initially ran faster
     * but then lost frames and aborted the control connection. */
    ret |= s->set_reg(s, 0x3036, 0xff, 0x60);
    ret |= s->set_reg(s, 0x3c07, 0xff, 0x08);
    ret |= s->set_reg(s, 0x3c09, 0xff, 0x1c);
    ret |= s->set_reg(s, 0x3c0a, 0xff, 0x9c);
    ret |= s->set_reg(s, 0x3c0b, 0xff, 0x40);
    ret |= s->set_reg(s, 0x3618, 0xff, 0x00);
    ret |= s->set_reg(s, 0x3612, 0xff, 0x29);
    ret |= s->set_reg(s, 0x3708, 0xff, 0x64);
    ret |= s->set_reg(s, 0x3709, 0xff, 0x52);
    ret |= s->set_reg(s, 0x370c, 0xff, 0x03);
    ret |= s->set_reg(s, 0x3a02, 0xffff, 0x03d8);
    ret |= s->set_reg(s, 0x3a14, 0xffff, 0x03d8);
    /* Ratio 2 is the highest stable cadence on these modules. Ratio 1 caused
     * severe Wi-Fi packet loss and intermittent control-plane starvation even
     * though the detector itself remained alive. */
    ret |= s->set_reg(s, 0x3824, 0xff, 0x02);
    if (!s_is_gray) {
        ret |= s->set_reg(s, 0x4001, 0xff, 0x02);
        /* 0x4004 is the BLC line count: how many black rows are averaged to
         * derive the per-column black-level offset. 0x02 is the minimum, so
         * the estimate is dominated by noise in those two rows and every
         * column keeps a slightly wrong offset - column fixed-pattern noise,
         * seen as vertical bars that are worst in dark areas.
         *
         * Measured on delivered frames as mean |adjacent column mean
         * difference| against the same for rows: a real scene varies about
         * equally both ways, but these read 2.3-3.3x higher horizontally.
         * 32 lines of averaging costs nothing at runtime - it is vertical
         * blanking the sensor spends anyway. */
        ret |= s->set_reg(s, 0x4004, 0xff, 0x20);
        ret |= s->set_reg(s, 0x4713, 0xff, 0x03);
        ret |= s->set_reg(s, 0x4407, 0xff, 0x04);
        ret |= s->set_reg(s, 0x460b, 0xff, 0x35);
        ret |= s->set_reg(s, 0x460c, 0xff, 0x22);
        ret |= s->set_reg(s, 0x5001, 0xff, 0xa3);
    }
    if (ret != 0) {
        ESP_LOGW(TAG, "OV5640 high-rate QVGA profile failed: %d", ret);
        return false;
    }
    if (!apply_mount_orientation(s_res)) return false;
    ESP_LOGI(TAG, "OV5640 high-rate QVGA profile active (%s)",
             s_is_gray ? "Y8" : "JPEG");
    return true;
}

/* PLL/timing must change in OV5640 software standby (0x3008 bit 6): rewritten
 * while streaming, ~1/3 of starts latched an internal black state (peak 4)
 * with identical register contents. */
static bool camera_enable_fast_tracking_ordinary_owned(void)
{
    sensor_t *s = s_inited ? esp_camera_sensor_get() : NULL;
    if (!s || !s->set_reg) return false;
    if (s->set_reg(s, 0x3008, 0x40, 0x40) != 0) return false;
    bool ok = camera_enable_fast_tracking_profile_owned();
    return s->set_reg(s, 0x3008, 0x40, 0x00) == 0 && ok;
}

bool camera_enable_fast_tracking(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_enable_fast_tracking_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_tracking_pclk_ordinary_owned(uint32_t requested_mhz, uint32_t *actual_mhz)
{
    if (actual_mhz) *actual_mhz = 0;
    if (!s_inited || !s_is_gray || s_res != CAM_RES_QVGA) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->id.PID != 0x5640 || !s->set_pll) return false;

    if (requested_mhz < 12) requested_mhz = 12;
    if (requested_mhz > 40) requested_mhz = 40;
    /* Espressif's OV5640 clock equation for this exact profile is:
     *   PCLK = XCLK * multiplier / 20
     * (sys_div=1, no root_2x, PCLK root /2, manual PCLK /4).
     * Derive the integer multiplier from the configured XCLK and cap the
     * result at the S3's 40 MHz DVP limit. This remains truthful for the
     * supported 8..27 MHz Kconfig range instead of only at XCLK=20 MHz. */
    const uint32_t xclk_mhz = CONFIG_FTMCS_CAM_XCLK_MHZ;
    uint32_t multiplier;
    uint32_t pclk_div = 4U;
    bool fast_sysclk = false;
#ifdef CONFIG_FTMCS_TRACKING_FAST_SYSCLK
    if (requested_mhz >= 40U) {
        /* Raise sensor cadence without exceeding the DVP input ceiling.
         * With 20 MHz XCLK, 8-bit mode and the qualified root tree,
         * multiplier=35 and PCLK divider=5 give VCO=700 MHz,
         * SCLK=87.5 MHz and PCLK=35 MHz. The 95/38 MHz trial lost
         * more complete frames; this intermediate profile keeps the
         * full-field QVGA timing and the validated CPU-copy path. */
        /* The 800 MHz optional trial is isolated from the 700 MHz baseline.
         * It is being requalified with the camera-copy task on core 0. */
        multiplier = ((s_perf_profile && s_qvga_pclk_div ? 800U : 700U) + xclk_mhz / 2U) / xclk_mhz;
        const uint32_t max_vco_multiplier = 800U / xclk_mhz;
        if (multiplier > max_vco_multiplier) multiplier = max_vco_multiplier;
        pclk_div = s_perf_profile && s_qvga_pclk_div ? s_qvga_pclk_div : 5U;
        fast_sysclk = true;
    } else
#endif
    {
        const uint32_t max_multiplier = (40U * 20U) / xclk_mhz;
        multiplier = (requested_mhz * 20U + xclk_mhz / 2U) / xclk_mhz;
        if (multiplier > max_multiplier) multiplier = max_multiplier;
    }
    if (multiplier < 4U) multiplier = 4U;
    const uint32_t actual_khz = fast_sysclk
        ? (xclk_mhz * multiplier * 1000U) / (4U * pclk_div)
        : (xclk_mhz * multiplier * 1000U) / (5U * pclk_div);
    const uint32_t sysclk_khz = fast_sysclk
        ? (xclk_mhz * multiplier * 1000U) / 8U
        : (xclk_mhz * multiplier * 1000U) / 10U;
    const uint32_t programmed_mhz = (actual_khz + 500U) / 1000U;
    int ret;
    if (fast_sysclk) {
        /* Register-level topology: pre-divider /1 and the qualified roots.
         * The upstream set_pll parameter names
         * are swapped by the OV5640 wrapper, so keep this separate from the
         * qualified standard call and verify every resulting register. */
        ret = s->set_pll(s, 0, (int)multiplier, 1, 0, 1,
                         1, 1, (int)pclk_div);
    } else {
        ret = s->set_pll(s, 0, (int)multiplier, 1, 0, 1,
                         1, 1, (int)pclk_div);
    }
    if (ret == 0 && fast_sysclk) {
        /* Change only the bit-mode nibble while preserving the register's
         * upper control bits and the qualified root tree from set_pll. */
        ret = s->set_reg(s, 0x3034, 0x0f, 0x08);
    }
    if (ret == 0 && fast_sysclk && s->get_reg) {
        const int pll0 = s->get_reg(s, 0x3034, 0xff);
        const int pll1 = s->get_reg(s, 0x3035, 0xff);
        const int pll2 = s->get_reg(s, 0x3036, 0xff);
        const int pll3 = s->get_reg(s, 0x3037, 0xff);
        const int root = s->get_reg(s, 0x3108, 0xff);
        const int div = s->get_reg(s, 0x3824, 0xff);
        if (pll0 < 0 || (pll0 & 0x0f) != 0x08 ||
            pll1 != 0x11 || pll2 != (int)multiplier || pll3 != 0x01 ||
            root != 0x16 || div != (int)pclk_div) {
            ESP_LOGE(TAG,
                     "fast tracking PLL readback failed: 3034=%02x 3035=%02x 3036=%02x 3037=%02x 3108=%02x 3824=%02x",
                     pll0, pll1, pll2, pll3, root, div);
            ret = -1;
        }
    }
    if (ret != 0) {
        ESP_LOGW(TAG, "tracking PLL request %lu MHz failed: %d",
                 (unsigned long)requested_mhz, ret);
        if (fast_sysclk) {
            /* Fail closed and restore the already-qualified 40 MHz baseline.
             * set_pll rewrites both the root tree and 10-bit mode, so a failed
             * experimental readback cannot leave a half-applied clock tree. */
            const uint32_t baseline_multiplier = (40U * 20U) / xclk_mhz;
            const int rollback = s->set_pll(s, 0, (int)baseline_multiplier,
                                            1, 0, 1, 1, 1, 4);
            ESP_LOGW(TAG, "tracking PLL rollback %s",
                     rollback == 0 ? "restored baseline" : "FAILED");
        }
        return false;
    }
    if (actual_mhz) *actual_mhz = programmed_mhz;
    s_tracking_sclk_khz = sysclk_khz;
    /* Retain the requested tier. The fast profile deliberately reports a
     * 35 MHz DVP clock for the 40 MHz tuning tier; storing the
     * value would silently fall back to the slower profile on the next mode
     * start. */
    s_tracking_pclk_mhz = requested_mhz;
    ESP_LOGI(TAG,
             "native-Y8 tracking clocks: requested_pclk=%lu multiplier=%lu pclk_div=%lu SYSCLK=%lu.%03lu PCLK=%lu.%03lu MHz profile=%s",
             (unsigned long)requested_mhz, (unsigned long)multiplier,
             (unsigned long)pclk_div,
             (unsigned long)(sysclk_khz / 1000U),
             (unsigned long)(sysclk_khz % 1000U),
             (unsigned long)(actual_khz / 1000U),
             (unsigned long)(actual_khz % 1000U),
             fast_sysclk ? "fast-sysclk" : "standard");
    return true;
}

bool camera_set_tracking_pclk(uint32_t requested_mhz, uint32_t *actual_mhz)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_tracking_pclk_ordinary_owned(requested_mhz, actual_mhz);
    camera_sensor_mutation_end();
    return result;
}

/* Tracking line length (HTS). Stock OV5640 QVGA timing = 2060 rows worth of
 * pixel clocks; 1556 is the short-line throughput profile that starves IR
 * marker exposure. Runtime knob: `cam rawhts <n>`. */
static uint16_t s_tracking_hts = 2060;

/* SCLK is recorded where each Y8 PLL profile is programmed; see the
 * declaration near the top of this file. */

/* Report what the sensor actually resolved, not what was requested. With AEC
 * and AGC on, the requested values read back as -1 (auto), so there is no way
 * to tell from the UI what exposure or gain the sensor settled on. These read
 * the OV5640's live registers: exposure is a 20-bit value in 1/16 row units
 * across 0x3500-0x3502, gain is 10 bits across 0x350a-0x350b in 1/16 steps. */
bool camera_read_live_exposure(uint32_t *out_rows, uint16_t *out_gain_q4,
                               uint16_t *out_hts, uint16_t *out_vts)
{
    if (!s_inited) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->get_reg) return false;
    int hi = s->get_reg(s, 0x3500, 0xff);
    int mid = s->get_reg(s, 0x3501, 0xff);
    int lo = s->get_reg(s, 0x3502, 0xff);
    int gain_hi = s->get_reg(s, 0x350a, 0xff);
    int gain_lo = s->get_reg(s, 0x350b, 0xff);
    int hts = s->get_reg(s, 0x380c, 0xffff);
    int vts = s->get_reg(s, 0x380e, 0xffff);
    if (hi < 0 || mid < 0 || lo < 0 || gain_hi < 0 || gain_lo < 0) return false;
    /* 0x3500[3:0]:0x3501[7:0]:0x3502[7:4] = exposure in rows, 4 fractional bits */
    uint32_t raw = ((uint32_t)(hi & 0x0f) << 16) | ((uint32_t)mid << 8) | (uint32_t)lo;
    if (out_rows) *out_rows = raw >> 4;
    if (out_gain_q4) *out_gain_q4 = (uint16_t)(((gain_hi & 0x03) << 8) | gain_lo);
    if (out_hts) *out_hts = hts > 0 ? (uint16_t)hts : 0;
    if (out_vts) *out_vts = vts > 0 ? (uint16_t)vts : 0;
    return true;
}

bool camera_set_tracking_hts(unsigned hts)
{
    if (hts < 800 || hts > 4095) return false;
    s_tracking_hts = (uint16_t)hts;
    return true;
}

/* Rolling-shutter row period in nanoseconds, 0 when unknown.
 *
 * OV5640 exposes rows sequentially, so a marker on row r is captured
 * r * row_period after row 0. At VGA (HTS 2060, SCLK 80 MHz) that is 25.75 us
 * per row and 12.4 ms across the frame - most of the host's 20 ms grouping
 * window - so this has to travel with the observation instead of being
 * assumed zero.
 *
 * Read from the live HTS register rather than s_tracking_hts: a preview
 * reprograms the sensor's line length, and a stale requested value would put
 * every marker timestamp wrong. The arithmetic check is the sensor's own
 * frame interval: row_period * VTS must match the measured source interval
 * (2060 * 984 / 80 MHz = 25.34 ms, measured 25.30-25.57 ms on node 0).
 *
 * Cached for a second because the readback is seven I2C transactions - about
 * 1 ms of a 28.6 ms tracking budget if it ran per frame. HTS only changes on
 * a mode/preview transition, so a stale second costs at most one second of
 * frames timestamped with the previous line length. */
/* SCLK from the sensor's own PLL registers.
 *
 * s_tracking_sclk_khz is only assigned by the tracking/fast-PLL paths, so in
 * plain preview or synchronized capture it stays 0 and the row period reported
 * 0 ns, which made every exposure-time display unavailable outside tracking.
 *
 * This mirrors the already-qualified formula used when programming the clock
 * (SCLK = XVCLK * multiplier / (8 * sys_div), see the native Y8 branch) rather
 * than a re-derived PLL model: same arithmetic, registers instead of locals.
 * The arithmetic check is the sensor's own frame interval, row_period * VTS
 * against the measured source interval. */
static uint32_t camera_sclk_khz_from_pll(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->get_reg) return 0;
    const int pll1 = s->get_reg(s, 0x3035, 0xff);   /* [7:4] sys_div */
    const int pll2 = s->get_reg(s, 0x3036, 0xff);   /* multiplier */
    if (pll1 < 0 || pll2 < 0 || pll2 == 0) return 0;
    uint32_t sys_div = (uint32_t)((pll1 >> 4) & 0x0f);
    if (sys_div == 0) sys_div = 16;
    return (uint32_t)((uint64_t)CONFIG_FTMCS_CAM_XCLK_MHZ * (uint32_t)pll2 * 1000ULL
                      / (8ULL * sys_div));
}

uint32_t camera_row_period_ns(void)
{
    static uint32_t cached_ns;
    static int64_t  cached_at_us;
    const int64_t now_us = esp_timer_get_time();
    if (cached_at_us && now_us - cached_at_us < 1000000) return cached_ns;

    uint32_t rows = 0;
    uint16_t gain = 0, hts = 0, vts = 0;
    cached_at_us = now_us;
    cached_ns = 0;
    /* Registers first: s_tracking_sclk_khz is a cache from whenever the
     * tracking PLL was last programmed, and a later preview/sync mode change
     * reprograms the clock without updating it. Trusting the cache reported
     * 112.5 MHz on a sensor actually running at 135 MHz (row 18311 ns vs a
     * true 15259 ns). The live registers cannot go stale. */
    uint32_t sclk_khz = camera_sclk_khz_from_pll();
    if (!sclk_khz) sclk_khz = s_tracking_sclk_khz;
    if (!sclk_khz) return 0;
    if (!camera_read_live_exposure(&rows, &gain, &hts, &vts) || !hts) return 0;
    cached_ns = (uint32_t)(((uint64_t)hts * 1000000ULL) / sclk_khz);
    return cached_ns;
}

unsigned camera_tracking_hts(void) { return s_tracking_hts; }

static bool camera_set_tracking_subsample_ordinary_owned(uint8_t factor)
{
    if (!s_inited || !s_is_gray || s_res != CAM_RES_QVGA ||
        (factor != 2 && factor != 4)) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->id.PID != 0x5640 || !s->set_res_raw || !s->set_reg) {
        return false;
    }

    /* OV5640 exposure is counted in row periods, so a short line collects
     * proportionally less light. HTS 1556 (vs the sensor's stock 2060) is a
     * throughput choice that costs 1.83x brightness and clamped the tracking
     * readout to a peak of ~133 no matter what exposure or gain was set, while
     * the preview path at stock timing saw the same scene peak at 248. The IR
     * marker sits above 200, so tracking could never reach its threshold.
     * Use the sensor's stock line length; `cam rawhts` trades it back. */
    uint16_t total_x = factor == 4 ? 800 : s_tracking_hts;
    uint16_t total_y = factor == 4 ? 500 : 1000;
    int ret = s->set_res_raw(s,
                             8, 2, 2615, 1953,
                             4, 2,
                             total_x, total_y,
                             320, 240,
                             true, true);
    /* set_res_raw's binning flag selects 0x31 (2x). The OV5640 increment
     * encoding 0x71 samples one of every four photosites while maintaining
     * the full sensor window; the ISP still scales to exact QVGA. */
    if (factor == 4) {
        ret |= s->set_reg(s, 0x3814, 0xff, 0x71);
        ret |= s->set_reg(s, 0x3815, 0xff, 0x71);
    }
    ret |= s->set_reg(s, 0x3a02, 0xffff, total_y);
    ret |= s->set_reg(s, 0x3a14, 0xffff, total_y);
    if (ret != 0) {
        ESP_LOGW(TAG, "native-Y8 %ux subsample profile failed: %d", factor, ret);
        return false;
    }
    ESP_LOGI(TAG, "native-Y8 sampling=%ux timing=%ux%u output=320x240",
             factor, total_x, total_y);
    return true;
}

bool camera_set_tracking_subsample(uint8_t factor)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_tracking_subsample_ordinary_owned(factor);
    camera_sensor_mutation_end();
    return result;
}

/* One full-field sensor envelope for EVERY resolution.
 *
 * This used to be two different things: QVGA got a complete profile (full
 * 1896x984 field, 2x binning, explicit PLL tier, DVP divider), while VGA/HD/
 * SXGA/FHD got a two-register fragment that raised the PLL and left the
 * geometry and DVP timing at whatever the driver had configured.
 *
 * That split is what broke 480p synchronized capture. Measured on node 0,
 * same scene, same commanded exposure of 128 rows:
 *
 *     QVGA  luma mean   7.0   (dark field, IR markers resolve as distinct spots)
 *     VGA   luma mean 224.5   (saturated wash, no features at all)
 *
 * and the VGA frame did not respond to the exposure control at all: sweeping
 * 128 -> 64 -> 32 -> 16 -> 8 rows and dropping gain 7.94x -> 0.94x moved the
 * mean only 224 -> 221. An exposure that does not change the image is not the
 * exposure the frame was taken with.
 *
 * The cause is the geometry half of the profile being skipped. Without the
 * explicit full-field window and the 0x3a02/0x3a14 AEC ceiling, the sensor
 * keeps the driver's own window and its own idea of the frame length, so the
 * FREX exposure - which is counted in row periods of THIS envelope - no longer
 * corresponds to the rows actually being read out.
 *
 * Programming the same envelope everywhere costs nothing (it is the timing the
 * sensor spends anyway) and makes behaviour predictable across modes: one
 * geometry, one PLL discipline, one exposure meaning. Resolution differs only
 * in the ISP output size. */
static bool camera_program_sensor_envelope(sensor_t *s,
                                           uint32_t requested_fps,
                                           uint16_t out_w, uint16_t out_h)
{
    /* Discrete, conservative PLL tiers. One of the three OV5640 modules
     * produced invalid entropy at the old all-or-nothing 0x60 multiplier, so
     * the rate ladder stays explicit rather than "fastest that links". */
    const uint8_t multiplier = requested_fps <= 50 ? 0x30 :
        requested_fps <= 60 ? 0x38 :
        requested_fps <= 90 ? 0x48 :
        requested_fps < 120 ? 0x50 : 0x60;

    /* FULL SENSOR FIELD AT THE OUTPUT'S ASPECT RATIO.
     *
     * A crop that narrows the field silently changes the effective intrinsics
     * after calibration, so every mode reads as much of the array as it can.
     * But the array is 2624x1944 (1.350, i.e. 4:3) and not every output is:
     *
     *     qvga 320x240   1.333    close enough (0.988x)
     *     vga  640x480   1.333    close enough (0.988x)
     *     hd   1280x720  1.778    1.317x HORIZONTAL STRETCH
     *     sxga 1280x1024 1.250    0.926x squeeze
     *     fhd  1920x1080 1.778    1.317x HORIZONTAL STRETCH
     *
     * Feeding the whole 4:3 window into a 16:9 frame is what made 720p look
     * stretched, and the uneven vertical resample that came with it is what
     * striped it: adjacent-row difference measured 2.24-2.35x the
     * adjacent-column difference at HD, against 0.75-1.23x at SXGA.
     *
     * So letterbox the READ WINDOW to the output's aspect instead of
     * distorting the picture: keep the full width when the output is wider
     * than the array, the full height when it is taller, and centre the
     * result. Pixels outside that band are genuinely not imaged, which is
     * honest and uniform, rather than imaged and then squashed.
     *
     * Calibration stays valid per mode because the window is deterministic
     * for a given output size - it is a property of the mode, not of runtime
     * state. */
    const uint16_t win_x0 = 0, win_y0 = 4;
    const uint16_t win_w = 2624, win_h = 1944;
    uint16_t crop_w = win_w;
    uint16_t crop_h = (uint16_t)(((uint32_t)win_w * out_h + out_w / 2) / out_w);
    if (crop_h > win_h) {
        crop_h = win_h;
        crop_w = (uint16_t)(((uint32_t)win_h * out_w + out_h / 2) / out_h);
    }
    const uint16_t start_x = (uint16_t)(win_x0 + (win_w - crop_w) / 2);
    const uint16_t start_y = (uint16_t)(win_y0 + (win_h - crop_h) / 2);
    const uint16_t end_x = (uint16_t)(start_x + crop_w - 1);
    const uint16_t end_y = (uint16_t)(start_y + crop_h - 1);

    /* Bin when the output fits in the binned readout: it costs a quarter of
     * the pixels per line for the same field of view. Binning a taller output
     * (SXGA 1024, FHD 1080 rows) would force the ISP to upscale, inventing
     * detail, so those read unbinned. */
    const bool bin = (out_w <= (crop_w / 2) && out_h <= (crop_h / 2));
    const uint16_t total_x = bin ? 1896 : 2844;
    const uint16_t total_y = bin ? 984 : 1968;

    int ret = s->set_res_raw(s,
                             start_x, start_y, end_x, end_y,
                             16, 6,
                             total_x, total_y,
                             out_w, out_h,
                             bin, bin);
    ret |= s->set_reg(s, 0x3035, 0xff, 0x14);
    ret |= s->set_reg(s, 0x3036, 0xff, multiplier);
    ret |= s->set_reg(s, 0x3c07, 0xff, 0x08);
    ret |= s->set_reg(s, 0x3c09, 0xff, 0x1c);
    ret |= s->set_reg(s, 0x3c0a, 0xff, 0x9c);
    ret |= s->set_reg(s, 0x3c0b, 0xff, 0x40);
    ret |= s->set_reg(s, 0x3618, 0xff, 0x00);
    ret |= s->set_reg(s, 0x3612, 0xff, 0x29);
    ret |= s->set_reg(s, 0x3708, 0xff, 0x64);
    ret |= s->set_reg(s, 0x3709, 0xff, 0x52);
    ret |= s->set_reg(s, 0x370c, 0xff, 0x03);
    /* AEC frame-length ceiling must match the envelope, or the exposure clamp
     * is computed against a frame length the sensor is not using. */
    ret |= s->set_reg(s, 0x3a02, 0xffff, total_y);
    ret |= s->set_reg(s, 0x3a14, 0xffff, total_y);
    /* 0x60 would drive the JPEG DVP output at roughly 48 MHz, above the
     * ESP32-S3 camera peripheral's qualified 40 MHz ceiling. A /3 output
     * divider keeps 120 Hz sensor timing while presenting about 32 MHz to DVP.
     * Lower tiers stay at /2. */
    ret |= s->set_reg(s, 0x3824, 0xff, requested_fps >= 120 ? 0x03 : 0x02);
    ret |= s->set_reg(s, 0x4001, 0xff, 0x02);
    /* BLC line count: 2 is the sensor minimum, so the per-column black level
     * is estimated from two noisy rows and every column keeps a slightly wrong
     * offset - column fixed-pattern noise, worst in dark areas. 32 lines of
     * averaging costs only vertical blanking the sensor spends anyway. */
    ret |= s->set_reg(s, 0x4004, 0xff, 0x20);
    ret |= s->set_reg(s, 0x4713, 0xff, 0x03);
    ret |= s->set_reg(s, 0x4407, 0xff, 0x04);
    ret |= s->set_reg(s, 0x460b, 0xff, 0x35);
    ret |= s->set_reg(s, 0x460c, 0xff, 0x22);
    ret |= s->set_reg(s, 0x5001, 0xff, 0xa3);
    if (ret != 0) {
        ESP_LOGW(TAG, "OV5640 envelope %ux%u failed: %d", out_w, out_h, ret);
        return false;
    }
    ESP_LOGI(TAG,
             "OV5640 envelope: request=%lu multiplier=0x%02x timing=%ux%u output=%ux%u",
             (unsigned long)requested_fps, multiplier, total_x, total_y,
             out_w, out_h);
    return true;
}

static bool camera_enable_high_rate_stream_ordinary_owned(uint32_t requested_fps)
{
    /* Y8 stays refused. Tried 2026-09-17: the 1600x500 turbo envelope programs
     * fine for JPEG but crashed the node when applied to QVGA Y8 (device
     * rebooted mid-start, uptime reset to 8 s). The ROI/binning registers this
     * writes are qualified only against the JPEG DVP path on these modules.
     * The raw path's lever is PCLK (see the VCO table in fast_tracking), not
     * this envelope. */
    if (!s_inited || s_is_gray) return false;

    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->id.PID != 0x5640 || !s->set_res_raw || !s->set_reg) {
        return false;
    }

    /* Every resolution gets the SAME full-field envelope.
     *
     * The previous split - a complete profile for QVGA, a two-register PLL
     * fragment for everything else - is what broke 480p synchronized capture:
     * the fragment raised the pixel clock without programming the geometry or
     * the AEC frame-length ceiling, so the commanded exposure stopped
     * corresponding to the rows actually read out. VGA sat at luma 224 and did
     * not respond to a 16x exposure sweep at all.
     *
     * An older note here warned that applying the full profile to VGA caused
     * vertical striping (column std 21.74 against row std 1.06, only 17
     * distinct luma levels). That measurement was taken with the driver's
     * unbinned window, where the ISP downscales 2623x1951 to 640x480 and the
     * scaler cannot retire a line at 1.71x pixel rate. This envelope bins 2x
     * first, so the scaler sees 1896x984 instead - roughly a quarter of the
     * pixels per line - and the timing it could not previously meet is no
     * longer being asked for.
     *
     * ponytail: striping is a per-COLUMN defect, so if bars return bisect
     * 0x3824 against 0x460b/0x460c rather than reverting the PLL; the rate is
     * not the cause. Verify with the row-mean/column-mean std ratio, not by
     * eye. */
    return camera_program_sensor_envelope(s, requested_fps, s_w, s_h);
}

bool camera_enable_high_rate_stream(uint32_t requested_fps)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_enable_high_rate_stream_ordinary_owned(requested_fps);
    camera_sensor_mutation_end();
    return result;
}


/* Register is fixed and typed; no arbitrary sensor-register console access.
 * 0x82 enables stationary COLOR squares (not monochrome squares). */
static bool square_pattern_write_checked(sensor_t *sensor, uint8_t value)
{
    if (sensor->set_reg(sensor, 0x503d, 0xff, value) != 0) return false;
    return sensor->get_reg(sensor, 0x503d, 0xff) == value;
}

static bool camera_set_stationary_color_squares_ordinary_owned(void)
{
    if (!s_inited) return false;
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV5640_PID ||
        !sensor->get_reg || !sensor->set_reg) return false;
    int previous = sensor->get_reg(sensor, 0x503d, 0xff);
    if (previous < 0 || previous > 0xff) return false;
    if (!s_square_pattern_active) {
        s_square_pattern_previous = (uint8_t)previous;
        s_square_pattern_previous_status = sensor->status.colorbar;
        s_square_pattern_active = true;
    }
    if (!square_pattern_write_checked(sensor, 0x82)) {
        /* A failed SCCB write may have changed the device. Attempt checked
         * restoration; retain pending state if restoration is not verified. */
        bool restored = square_pattern_write_checked(sensor, s_square_pattern_previous);
        if (restored) {
            sensor->status.colorbar = s_square_pattern_previous_status;
            s_square_pattern_active = false;
        }
        ESP_LOGW(TAG, "stationary color squares failed; previous register restored=%d", restored);
        return false;
    }
    sensor->status.colorbar = 1;
    ESP_LOGI(TAG, "stationary color squares active: 0x503d readback=0x82");
    return true;
}

bool camera_set_stationary_color_squares(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_stationary_color_squares_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_test_pattern_ordinary_owned(bool enabled)
{
    if (!s_inited) return false;
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_colorbar) return false;
    if (s_square_pattern_active) {
        if (sensor->id.PID != OV5640_PID || !sensor->get_reg || !sensor->set_reg ||
            !square_pattern_write_checked(sensor, s_square_pattern_previous)) return false;
        sensor->status.colorbar = s_square_pattern_previous_status;
        s_square_pattern_active = false;
    }
    /* Retain the original driver's on/off semantics. It masks only0xc0;
     * restoring first prevents squares -> on from becoming rolling squares. */
    return sensor->set_colorbar(sensor, enabled ? 1 : 0) == 0;
}

bool camera_set_test_pattern(bool enabled)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_test_pattern_ordinary_owned(enabled);
    camera_sensor_mutation_end();
    return result;
}


static unsigned s_preview_vco_mhz = 0; /* Auto: QVGA 800, larger JPEG 560. */
#include "ftmcs_camera_pipeline_diag.h"
bool camera_set_pipeline_diagnostics(bool enabled)
{
    if (s_inited) return false;
    ftmcs_camera_pipeline_diag_enable(enabled);
    return true;
}
void camera_print_profile(void)
{
    printf("camera profile: initialized=%u pending=%u buffers=%u raw_buffers_requested=%u perf=%u raw_vco=%u jpeg_vco=%u qvga_div=%u jpeg_buffers_requested=%u task_core=%d dma_buffer=%u dma_chunk=%u direct_dma=%u jpeg_hts_requested=%u preview_hts=%u psram_mhz=%u raw_ring_kib=%u raw_chunk_requested=%u\n",
           (unsigned)s_inited, (unsigned)s_profile_dirty, s_driver_buffer_count,
           s_raw_buffer_count, (unsigned)s_perf_profile, s_tracking_vco_mhz,
           s_preview_vco_mhz, s_qvga_pclk_div, s_jpeg_buffer_count, s_driver_task_core,
           (unsigned)s_dma_buffer_bytes, (unsigned)s_dma_chunk_bytes, (unsigned)s_direct_dma,
           s_preview_hts, s_effective_preview_hts, (unsigned)CONFIG_SPIRAM_SPEED, s_raw_dma_ring_kib, camera_get_raw_dma_chunk());
    ftmcs_camera_pipeline_diag_t d;
    ftmcs_camera_pipeline_diag_read(&d);
    printf("capture counters: enabled=%u time_us=%lld vsync_events=%lu eof=%lu event_overflow=%lu started=%lu no_buffer=%lu raw_size_reject=%lu queue_replace=%lu returned=%lu\n",
           (unsigned)d.enabled, (long long)d.time_us,
           (unsigned long)d.count[FTMCS_CAM_VSYNC], (unsigned long)d.count[FTMCS_CAM_EOF],
           (unsigned long)d.count[FTMCS_CAM_EVENT_OVERFLOW], (unsigned long)d.count[FTMCS_CAM_STARTED],
           (unsigned long)d.count[FTMCS_CAM_NO_BUFFER], (unsigned long)d.count[FTMCS_CAM_RAW_SIZE_REJECT],
           (unsigned long)d.count[FTMCS_CAM_QUEUE_REPLACE], (unsigned long)d.count[FTMCS_CAM_RETURNED]);
    printf("capture rejects: last_bytes=%lu expected_bytes=%lu chunk_bytes=%lu eof_chunks=%lu empty=%lu short_one_chunk=%lu other=%lu\n",
           (unsigned long)d.reject.last_bytes, (unsigned long)d.reject.expected_bytes,
           (unsigned long)d.reject.chunk_bytes, (unsigned long)d.reject.eof_chunks,
           (unsigned long)d.reject.empty, (unsigned long)d.reject.short_one_chunk,
           (unsigned long)d.reject.other);
}

static bool camera_set_preview_vco_ordinary_owned(unsigned mhz)
{
    if (mhz != 0 && (mhz < 560 || mhz > 800 || mhz % CONFIG_FTMCS_CAM_XCLK_MHZ)) return false;
    s_profile_dirty = true;
    s_preview_vco_mhz = mhz;
    return true;
}

bool camera_set_preview_vco(unsigned mhz)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_preview_vco_ordinary_owned(mhz);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_enable_perf_preview_ordinary_owned(void)
{
    if (!s_perf_profile) return true;
    if (!s_inited || s_is_gray) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->id.PID != 0x5640 || !s->set_pll || !s->get_reg || !s->set_reg) return false;
    /* VCO 560 MHz for every preview resolution.
     *
     * This used to select 800 MHz for QVGA/VGA and 560 only for HD, which is
     * precisely why "240p and 480p are black but 720p is fine": a faster sensor
     * clock shortens the row period, and OV5640 exposure is counted in row
     * periods, so the low-resolution previews were starved of light while HD -
     * the only mode already on 560 - stayed correctly exposed.
     *
     * Measured on node 0, same scene, decoded luminance (0-255):
     *   480p  VCO 800 -> mean   7.0, 38 distinct values   (black)
     *   480p  VCO 560 -> mean 139.5, 256 distinct values  (correct)
     *   240p  VCO 800 -> mean  15.8
     *   240p  VCO 560 -> mean  62.9, 255 distinct values
     *
     * `cam jpegclock <mhz>` still overrides it for throughput experiments. */
    const unsigned selected_vco = s_preview_vco_mhz ? s_preview_vco_mhz : 560U;
    const int multiplier = selected_vco / CONFIG_FTMCS_CAM_XCLK_MHZ;
    /* JPEG bursts are shorter than full Y8 rows. Retain full-field crop and
     * binning while separating sensor cadence from the bounded DVP clock. */
    int ret = s->set_pll(s, 0, multiplier, 1, 0, 1, 1, 1, 5);
    ret |= s->set_reg(s, 0x3034, 0x0f, 8);
    /* Short-line VGA with VCO 800 passed the sensor-pattern gate (2026-09-15,
     * test-output/pipeline/20260915-212921-sensor-pattern: bars crisp, zero
     * validation errors) and lifts VGA JPEG from ~17 to ~64 FPS. It is now
     * part of the perf profile default, not a diagnostic. */
    /* Short lines raise FPS but shorten the row period, and OV5640 exposure is
     * counted in row periods — so HTS 1556 collects 1.83x less light per row
     * than the stock ~2844. That starved QVGA/VGA preview into a black frame
     * while HD (never short-lined) stayed correctly exposed, which is exactly
     * the "240p/480p black, 720p fine" report. A preview the user cannot see
     * is worth less than a faster one, so default to the sensor's own timing
     * and keep the short-line profile available via `cam jpeghts 1556`. */
    const unsigned requested_hts = s_preview_hts;
    if (requested_hts && (s_res == CAM_RES_QVGA || s_res == CAM_RES_VGA))
        ret |= s->set_reg(s, 0x380c, 0xffff, (int)requested_hts);
    int actual_hts = s->get_reg(s, 0x380c, 0xffff);
    /* Only verify the override when one was actually requested; the sensor's
     * own default HTS is whatever its mode table programmed. */
    if (actual_hts <= 0 || (requested_hts && (s_res == CAM_RES_QVGA ||
        s_res == CAM_RES_VGA) && (unsigned)actual_hts != requested_hts)) return false;
    s_effective_preview_hts = (unsigned)actual_hts;
    /* QVGA JPEG VTS reduction (500/700) was tested 2026-09-16 and produced
     * zero frames both times (stream timeout) — the sensor/AEC/DVP chain on
     * these modules cannot run the JPEG path below 984 rows. QVGA ~65 FPS
     * is the stable ceiling; VGA already matches it at 4x pixels. */
    if (ret || (s->get_reg(s, 0x3034, 0xff) & 15) != 8 ||
        s->get_reg(s, 0x3035, 0xff) != 0x11 || s->get_reg(s, 0x3036, 0xff) != multiplier ||
        s->get_reg(s, 0x3037, 0xff) != 1 || s->get_reg(s, 0x3108, 0xff) != 0x16 ||
        s->get_reg(s, 0x3824, 0xff) != 5) return false;
    ESP_LOGI(TAG, "full-field JPEG VCO=%u MHz SCLK=%u kHz PCLK=%u kHz", selected_vco,
             selected_vco * 1000 / 8, selected_vco * 1000 / 20);
    return true;
}

bool camera_enable_perf_preview(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_enable_perf_preview_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

static bool camera_set_preview_hts_ordinary_owned(unsigned hts)
{
    /* 0 = sensor default. 1556 is the short-line FPS profile; longer lines
     * trade frame rate for exposure time, which IR marker capture needs more
     * than speed. Row period is HTS/PCLK, so exposure scales linearly with
     * HTS: 1556 collects 1.83x less light per row than the stock ~2844. */
    if (hts != 0 && (hts < 1556 || hts > 4095)) return false;
    s_preview_hts = hts;
    s_profile_dirty = true;
    return true;
}

bool camera_set_preview_hts(unsigned hts)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_set_preview_hts_ordinary_owned(hts);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_check_raw_pattern_ordinary_owned(cam_res_t resolution)
{
    bool ok = false;
    const uint8_t *pixels = NULL;
    size_t length = 0;
    int stride = 0;
    int64_t stamp = 0, previous = -1;
    uint16_t means[4][8] = {0};
    camera_stop();
    if (!camera_start_grayscale(resolution) || !camera_config_continuous_mode() || !camera_enable_fast_tracking() ||
        !camera_apply_tuning() || !camera_set_test_pattern(true)) goto cleanup;
    /* Flush queued frames and sensor configuration latency using distinct frames. */
    for (unsigned fresh = 0, attempts = 0; fresh < 6 && attempts < 12; ++attempts) {
        pixels = camera_grab_gray_quiet(&length, &stride, &stamp);
        if (!pixels) goto cleanup;
        if (stamp != previous) { ++fresh; previous = stamp; }
        camera_release(pixels); pixels = NULL;
        if (attempts == 11 && fresh < 6) goto cleanup;
    }
    pixels = camera_grab_gray_quiet(&length, &stride, &stamp);
    if (!pixels || stride != 1 || length != (size_t)s_w * s_h) goto cleanup;
    ok = true;
    for (unsigned band = 0; band < 4; ++band) {
        unsigned lo = 255, hi = 0;
        for (unsigned column = 0; column < 8; ++column) {
            uint32_t sum = 0, count = 0;
            for (unsigned y = band * s_h / 4; y < (band + 1) * s_h / 4; ++y)
                for (unsigned x = column * s_w / 8; x < (column + 1) * s_w / 8; ++x) {
                    sum += pixels[(size_t)y * s_w + x]; ++count;
                }
            unsigned mean = count ? sum / count : 0;
            means[band][column] = mean;
            if (mean < lo) lo = mean;
            if (mean > hi) hi = mean;
        }
        if (hi - lo < 80) ok = false;
    }
    unsigned max_shift = 0;
    for (unsigned column = 0; column < 8; ++column) {
        unsigned lo = 255, hi = 0;
        for (unsigned band = 0; band < 4; ++band) {
            if (means[band][column] < lo) lo = means[band][column];
            if (means[band][column] > hi) hi = means[band][column];
        }
        if (hi - lo > max_shift) max_shift = hi - lo;
    }
    if (max_shift > 30) ok = false;
    printf("raw pattern: width=%u height=%u pixel_stride=%d bytes=%u max_band_shift=%u\n",
           s_w, s_h, stride, (unsigned)length, max_shift);
    for (unsigned band = 0; band < 4; ++band)
        printf("raw pattern band%u: %u %u %u %u %u %u %u %u\n", band,
               means[band][0], means[band][1], means[band][2], means[band][3],
               means[band][4], means[band][5], means[band][6], means[band][7]);
    camera_print_profile();
cleanup:
    if (pixels) camera_release(pixels);
    if (s_inited && !camera_set_test_pattern(false)) ok = false;
    camera_stop();
    return ok;
}

bool camera_check_raw_pattern(cam_res_t resolution)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_check_raw_pattern_ordinary_owned(resolution);
    camera_sensor_mutation_end();
    return result;
}

static bool camera_run_deferred_eof_ordinary_owned(unsigned wait_us)
{
    if (!ftmcs_deferred_delay_valid(wait_us) || s_inited ||
        strobe_gpio_get_mode()!=STROBE_OFF || ota_server_uploading()) return false;
    /* Discover only before arming. Preserve the known preset across teardown;
     * raw initialization then uses that exact preset, not another live probe. */
    cam_detect_result_t found=camera_detect();
    int preset=s_preset;
    camera_stop();
    if (!found.ok || found.pid!=0x5640 || preset<1 || preset>PRESET_COUNT) return false;
    s_preset=preset;
    bool prepared=ftmcs_camera_deferred_prepare(wait_us);
    bool configured=prepared && reinit_camera(CAM_RES_QVGA,PIXFORMAT_GRAYSCALE) &&
        camera_enable_fast_tracking() && camera_apply_tuning();
    bool activated=configured && !ota_server_uploading() && ftmcs_camera_deferred_activate();
    ftmcs_deferred_result_t result={0};
    const int64_t episode_deadline=esp_timer_get_time()+2000000;
    if (!activated) ftmcs_camera_deferred_abort(FTMCS_DEFER_ABORT);
    while (activated) {
        ftmcs_camera_deferred_result(&result);
        if (result.end_task_us) break;
        if (ota_server_uploading()) ftmcs_camera_deferred_abort(FTMCS_DEFER_ABORT);
        if (esp_timer_get_time()>=episode_deadline) {
            ftmcs_camera_deferred_abort(FTMCS_DEFER_EPISODE_TIMEOUT);
            break;
        }
        vTaskDelay(1);
    }
    /* Full teardown joins copier and fences IRQs before freeing queues. A
     * failed fence deliberately aborts, never reuses a potentially owned ring. */
    camera_stop();
    ftmcs_camera_deferred_disarm();
    ftmcs_camera_deferred_result(&result);
    printf("deferred_eof wait_us=%u reason=%u rejected=%u bytes=%u chunks=%u epoch=%u eof_serial=%u trace_serial=%u first_irq_kind=%u first_irq_serial=%u first_irq_cycles=%u first_irq_core=%u eof_seen=%u next_vsync=%u queue_errors=%u boundary_task_us=%lld deadline_task_us=%lld stop_task_us=%lld end_task_us=%lld teardown_ok=%u attribution=ambiguous frames_published=0\n",
        (unsigned)result.wait_us,(unsigned)result.reason,(unsigned)result.rejected,
        (unsigned)result.reject_bytes,(unsigned)result.reject_chunks,
        (unsigned)result.boundary_epoch,(unsigned)result.boundary_eof_serial,(unsigned)result.boundary_trace_serial,
        (unsigned)result.first_irq_kind,(unsigned)result.first_irq_serial,
        (unsigned)result.first_irq_cycles,(unsigned)result.first_irq_core,
        (unsigned)result.irq_eof_seen,(unsigned)result.next_vsync,(unsigned)result.queue_errors,
        (long long)result.boundary_task_us,(long long)result.deadline_task_us,
        (long long)result.stop_task_us,(long long)result.end_task_us,(unsigned)result.teardown_ok);
    return activated && result.rejected && result.teardown_ok &&
        result.reason>=FTMCS_DEFER_ZERO && result.reason<=FTMCS_DEFER_TIMEOUT;
}

bool camera_run_deferred_eof(unsigned wait_us)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = camera_run_deferred_eof_ordinary_owned(wait_us);
    camera_sensor_mutation_end();
    return result;
}


typedef struct { sensor_t *sensor; } camera_cip_lease_t;
static bool camera_cip_enter(void *context, cip_guard *guard)
{
    if (!sensor_lifetime_take()) return false;
    camera_cip_lease_t *lease = context;
    lease->sensor = s_sensor_lifetime_ready ? esp_camera_sensor_get() : NULL;
    guard->generation = s_sensor_generation;
    guard->pid = lease->sensor ? lease->sensor->id.PID : 0;
    guard->exclusive_setters = !s_sensor_teardown_unconfirmed;
    guard->squares = lease->sensor && lease->sensor->get_reg &&
        !frex_sync_active() && s_square_pattern_active && lease->sensor->get_reg(lease->sensor, 0x503d, 0xff) == 0x82;
    if (!lease->sensor || !lease->sensor->get_reg || !lease->sensor->set_reg) {
        lease->sensor = NULL;
        sensor_lifetime_give();
        return false;
    }
    return true;
}
static void camera_cip_leave(void *context)
{
    camera_cip_lease_t *lease = context;
    lease->sensor = NULL;
    sensor_lifetime_give();
}
static int camera_cip_read(void *context, uint16_t address)
{
    camera_cip_lease_t *lease = context;
    return lease->sensor->get_reg(lease->sensor, address, 0xff);
}
static int camera_cip_write(void *context, uint16_t address, uint8_t value)
{
    camera_cip_lease_t *lease = context;
    return lease->sensor->set_reg(lease->sensor, address, 0xff, value);
}
static cip_status camera_cip_restore_owned(void)
{
    if (s_cip.phase == CIP_IDLE) return CIP_OK;
    camera_cip_lease_t lease = {0};
    cip_io io = {&lease, camera_cip_enter, camera_cip_leave, camera_cip_read, camera_cip_write};
    return cip_restore(&s_cip, &io);
}
bool camera_cip_command(camera_cip_action_t action)
{
    if (action < CAMERA_CIP_MATCH || action > CAMERA_CIP_EDGE_RETURN) return false;
    if (!sensor_lifetime_take()) return false;
    camera_cip_lease_t lease = {0};
    cip_io io = {&lease, camera_cip_enter, camera_cip_leave, camera_cip_read, camera_cip_write};
    cip_phase previous = s_cip.phase;
    cip_status status;
    switch (action) {
    case CAMERA_CIP_MATCH: status = cip_match(&s_cip, &io); break;
    case CAMERA_CIP_RESTORE: status = cip_restore(&s_cip, &io); break;
    case CAMERA_CIP_EDGE_PLUS_ONE: status = cip_edge_plus_one(&s_cip, &io); break;
    case CAMERA_CIP_EDGE_RETURN: status = cip_edge_return(&s_cip, &io); break;
    default: sensor_lifetime_give(); return false;
    }
    cip_state record = s_cip;
    sensor_lifetime_give();
    const bool restoring = action == CAMERA_CIP_RESTORE;
    const char *state = action == CAMERA_CIP_MATCH ? "matched" :
        action == CAMERA_CIP_EDGE_PLUS_ONE ? "edgeplus1" :
        action == CAMERA_CIP_EDGE_RETURN ? "edgereturn" :
        previous == CIP_IDLE ? "idle" : "restored";
    if (status == CIP_OK && !restoring)
        printf("cip state=%s generation=%llu restore_pending=1 edge=%u denoise=%u sharpen=%u\n",
            state, (unsigned long long)record.generation,(unsigned)record.temporary_edge,
            (unsigned)record.sampled[1],(unsigned)record.sampled[2]);
    else if (status == CIP_OK)
        printf("cip state=%s generation=%llu restore_pending=0\n", state,
            (unsigned long long)record.generation);
    else
        printf("cip state=failed generation=%llu restore_pending=%u\n",
            (unsigned long long)record.generation,(unsigned)(record.phase != CIP_IDLE));
    printf("cip_detail status=%u primary=%u address=0x%04x value=%d rollback_write=%u rollback_read=%u rollback_mismatch=%u failures=%llu abandoned=%u\n",
        (unsigned)status,(unsigned)record.primary_error,(unsigned)record.primary_address,record.primary_value,
        (unsigned)record.restore_write_errors,(unsigned)record.restore_read_errors,(unsigned)record.restore_mismatches,
        (unsigned long long)record.failure_count,(unsigned)record.ever_abandoned);
    return status == CIP_OK;
}

typedef struct {
    sensor_register_snapshot_t snapshot;
    sensor_snapshot_status_t status;
    uint16_t failed_address,pid;
} deferred_content_profile_t;
/* Caller owns the whole ordinary-operation recursive lifetime lease. No
 * sensor pointer survives this function. Pattern/CIP setup is already done. */
static bool deferred_content_profile_collect(deferred_content_profile_t *out)
{
    out->status=SENSOR_SNAPSHOT_INVALID;
    sensor_t *sensor=s_sensor_lifetime_ready ? esp_camera_sensor_get() : NULL;
    if (!sensor || !sensor->get_reg || sensor->id.PID!=OV5640_PID) return false;
    out->pid=sensor->id.PID;
    out->status=sensor_snapshot_collect(sensor,sensor_snapshot_read_byte,
        sensor_snapshot_now,&out->snapshot,&out->failed_address);
    return out->status==SENSOR_SNAPSHOT_OK;
}
static void deferred_content_profile_print(const char *phase,bool valid,
                                           const deferred_content_profile_t *out)
{
    printf("deferred_content_isp phase=%s valid=%u status=%d failed_reg=%u pid=%u snapshot_nonatomic=1\n",
        phase,(unsigned)valid,(int)out->status,(unsigned)out->failed_address,(unsigned)out->pid);
    if (!valid) return;
    printf("deferred_content_isp_time phase=%s before_us=%lld after_us=%lld\n",phase,
        (long long)out->snapshot.before_us,(long long)out->snapshot.after_us);
    for(unsigned i=0;i<SENSOR_SNAPSHOT_COUNT;++i)
        printf("deferred_content_isp_reg phase=%s address=%u value=%u\n",phase,
            (unsigned)sensor_snapshot_addresses[i],(unsigned)out->snapshot.values[i]);
}

static bool deferred_content_print(void)
{
    const ftmcs_deferred_content_t *r=ftmcs_camera_deferred_content_result();
    if (!r || !r->valid) {
        printf("deferred_content valid=0 error=%u\n",r?(unsigned)r->error:3u);
        return false;
    }
    printf("deferred_content valid=1 epoch=%u frame_index=%u bytes=%u chunk=%u ring=%u next_slot=%u count=%u spatial_only=1 temporal_identity=unknown\n",
        (unsigned)r->epoch,(unsigned)r->frame_index,(unsigned)r->rejected_bytes,
        (unsigned)r->chunk_bytes,(unsigned)r->ring_bytes,(unsigned)r->next_slot,(unsigned)r->count);
    for(unsigned i=0;i<r->count;++i) {
        const ftmcs_content_digest_t *d=&r->digest[i];
        printf("deferred_content_hash kind=%u index=%u offset=%u length=%u sha256=",
            (unsigned)d->kind,(unsigned)d->index,(unsigned)d->offset,(unsigned)d->length);
        for(unsigned j=0;j<32;++j)printf("%02x",(unsigned)d->sha256[j]);
        printf("\n");
    }
    return true;
}

static bool camera_run_deferred_content_ordinary_owned(unsigned wait_us)
{
    if (!ftmcs_deferred_delay_valid(wait_us) || s_inited ||
        strobe_gpio_get_mode()!=STROBE_OFF || ota_server_uploading()) return false;
    /* Discover only before arming. Preserve the known preset across teardown;
     * raw initialization then uses that exact preset, not another live probe. */
    cam_detect_result_t found=camera_detect();
    int preset=s_preset;
    camera_stop();
    if (!found.ok || found.pid!=0x5640 || preset<1 || preset>PRESET_COUNT) return false;
    s_preset=preset;
    bool prepared=ftmcs_camera_deferred_prepare(wait_us);
    if (prepared) prepared=ftmcs_camera_deferred_content_enable();
    bool configured=prepared && reinit_camera(CAM_RES_QVGA,PIXFORMAT_GRAYSCALE) &&
        camera_enable_fast_tracking() && camera_apply_tuning() &&
        camera_set_stationary_color_squares();
    deferred_content_profile_t before={0},after={0};
    bool before_ok=configured && deferred_content_profile_collect(&before);
    configured=configured && before_ok;
    bool activated=configured && !ota_server_uploading() && ftmcs_camera_deferred_activate();
    ftmcs_deferred_result_t result={0};
    const int64_t episode_deadline=esp_timer_get_time()+2000000;
    if (!activated) ftmcs_camera_deferred_abort(FTMCS_DEFER_ABORT);
    while (activated) {
        ftmcs_camera_deferred_result(&result);
        if (result.end_task_us) break;
        if (ota_server_uploading()) ftmcs_camera_deferred_abort(FTMCS_DEFER_ABORT);
        if (esp_timer_get_time()>=episode_deadline) {
            ftmcs_camera_deferred_abort(FTMCS_DEFER_EPISODE_TIMEOUT);
            break;
        }
        vTaskDelay(1);
    }
    bool after_ok=configured && deferred_content_profile_collect(&after);
    /* Full teardown joins copier and fences IRQs before freeing queues. A
     * failed fence deliberately aborts, never reuses a potentially owned ring. */
    camera_stop();
    ftmcs_camera_deferred_disarm();
    bool content_ok=deferred_content_print();
    deferred_content_profile_print("before_warmup",before_ok,&before);
    deferred_content_profile_print("before_teardown",after_ok,&after);
    ftmcs_camera_deferred_result(&result);
    printf("deferred_eof wait_us=%u reason=%u rejected=%u bytes=%u chunks=%u epoch=%u eof_serial=%u trace_serial=%u first_irq_kind=%u first_irq_serial=%u first_irq_cycles=%u first_irq_core=%u eof_seen=%u next_vsync=%u queue_errors=%u boundary_task_us=%lld deadline_task_us=%lld stop_task_us=%lld end_task_us=%lld teardown_ok=%u attribution=ambiguous frames_published=0\n",
        (unsigned)result.wait_us,(unsigned)result.reason,(unsigned)result.rejected,
        (unsigned)result.reject_bytes,(unsigned)result.reject_chunks,
        (unsigned)result.boundary_epoch,(unsigned)result.boundary_eof_serial,(unsigned)result.boundary_trace_serial,
        (unsigned)result.first_irq_kind,(unsigned)result.first_irq_serial,
        (unsigned)result.first_irq_cycles,(unsigned)result.first_irq_core,
        (unsigned)result.irq_eof_seen,(unsigned)result.next_vsync,(unsigned)result.queue_errors,
        (long long)result.boundary_task_us,(long long)result.deadline_task_us,
        (long long)result.stop_task_us,(long long)result.end_task_us,(unsigned)result.teardown_ok);
    return activated && content_ok && before_ok && after_ok && result.rejected && result.teardown_ok &&
        result.reason>=FTMCS_DEFER_ZERO && result.reason<=FTMCS_DEFER_TIMEOUT;
}
bool camera_run_deferred_content(unsigned wait_us)
{
    if (!camera_sensor_mutation_begin()) return false;
    bool result=camera_run_deferred_content_ordinary_owned(wait_us);
    camera_sensor_mutation_end();
    return result;
}

bool camera_set_startup_vsync_only(bool enabled)
{
    if (!camera_sensor_mutation_begin()) return false;
    bool ok=!s_inited && !s_sensor_lifetime_ready && !s_sensor_teardown_unconfirmed &&
        strobe_gpio_get_mode()==STROBE_OFF && !ota_server_uploading() &&
        ftmcs_camera_startup_vsync_set(enabled);
    camera_sensor_mutation_end();
    return ok;
}

bool camera_get_startup_vsync_only(bool *enabled)
{
    if (!enabled || !sensor_lifetime_take()) return false;
    *enabled=ftmcs_camera_startup_vsync_get();
    sensor_lifetime_give();
    return true;
}

static unsigned s_jpeg_mode_requested;
bool camera_set_jpeg_mode_requested(unsigned mode)
{
    /* Called only by OFF mode owner while holding sensor mutation lease. */
    if ((mode != 0 && mode != 2 && mode != 3) || camera_running()) return false;
    s_jpeg_mode_requested = mode;
    return true;
}
unsigned camera_get_jpeg_mode_requested(void) { return s_jpeg_mode_requested; }
static bool camera_apply_jpeg_mode_ordinary_owned(void)
{
    if (!s_jpeg_mode_requested) return true; /* Preserve per-resolution defaults. */
    if (!s_inited || s_is_gray) return false;
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV5640_PID || !sensor->set_reg || !sensor->get_reg) return false;
    int before = sensor->get_reg(sensor, 0x4713, 0xff);
    if (before < 0 || before > 255) return false;
    int expected = (before & ~7) | (int)s_jpeg_mode_requested;
    if (sensor->set_reg(sensor, 0x4713, 0x07, s_jpeg_mode_requested) != 0) return false;
    int actual = sensor->get_reg(sensor, 0x4713, 0xff);
    return actual == expected;
}
bool camera_apply_jpeg_mode_preview(void)
{
    if (!camera_sensor_mutation_begin()) return false;
    bool ok = camera_apply_jpeg_mode_ordinary_owned();
    camera_sensor_mutation_end();
    return ok;
}

bool camera_get_jpeg_mode_snapshot(unsigned *mode, uint64_t *generation, bool *ready)
{
    /* Caller retains mode owner while this takes the sensor lifetime lock. */
    if (!mode || !generation || !ready || !sensor_lifetime_take()) return false;
    *mode = s_jpeg_mode_requested;
    *generation = s_sensor_generation;
    *ready = s_sensor_lifetime_ready;
    sensor_lifetime_give();
    return true;
}
