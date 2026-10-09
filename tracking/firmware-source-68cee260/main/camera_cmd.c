/*
 * camera_cmd.c -- see camera_cmd.h.
 *
 * Console commands:
 *   cam detect                 probe all pin presets, report sensor
 *   cam on [qvga|vga|sxga] [gray] start TCP raw-grayscale preview
 *   cam off                    stop server + camera
 *   cam exp <level>            manual exposure (-1 = auto)
 *   cam stats                  show UVC + tracking performance counters
 *   cam selftest               verify exact packed centroid thresholding
 *   cam rate <12..40>          set native-Y8 QVGA PCLK while tracking is stopped
 *   cam scan <2|4>             set native-Y8 sensor subsampling envelope
 *   intrinsic get [qvga|vga|sxga]   print stored K,D
 *   intrinsic clear [qvga|vga|sxga] erase stored intrinsic
 *   intrinsic set <res> <K:9> <D:8> <w> <h> <pid> <reproj> <hmirror> <vflip>
 *                             (used by the PC calibration tool; K and D are
 *                              comma-separated doubles)
 */
#include "camera_cmd.h"

// The camera subsystem is always enabled on the thesis ESP32-S3 boards (all
// have PSRAM + a DVP sensor). The original Kconfig guard
// (#if CONFIG_FTMCS_CAMERA_ENABLE) was being dropped by the build system
// despite being =y in sdkconfig, so the command never registered. The
// hardware is fixed, so compile the camera console commands unconditionally.
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/temperature_sensor.h"
#include "esp32s3/rom/cache.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_private/cache_utils.h"
#include "soc/spi_mem_reg.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "camera_capture.h"
#include "camera_frex_sync.h"
#include "vsync_stamp.h"
#include "camera_server.h"
#include "camera_udp.h"
#include "centroid_packet.h"
#include "telemetry.h"
#include "lan_cmd.h"
#include "mode_ctrl.h"
#include "ota_server.h"
#include "strobe_gpio.h"
#include "sync_capture.h"
#include "intrinsic_store.h"
#include "live_track.h"
#include "uvc_webcam.h"

static const char *TAG = "camera_cmd";

/* ---- cam command (subcommand-style via first string arg) ---- */
static struct {
    struct arg_str *sub;
    struct arg_str *opts;
    struct arg_int *a1i;
    struct arg_end *end;
} cam_args;


typedef struct { size_t words; unsigned passes, errors, reread_ok, first_off; uint32_t got, want, xor_bits; bool failed; SemaphoreHandle_t done; } memtest_job_t;
static void memtest_run(memtest_job_t *j)
{
    uint32_t *m = heap_caps_malloc(j->words * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) { j->failed = true; return; }
    for (unsigned p = 0; p < j->passes; ++p) {
        uint32_t seed = 0x9e3779b9u * (p + 1);
        for (size_t i = 0; i < j->words; ++i) m[i] = (uint32_t)(uintptr_t)&m[i] ^ seed ^ (uint32_t)(i * 0x01000193u);
        for (size_t i = 0; i < j->words; ++i) {
            uint32_t want = (uint32_t)(uintptr_t)&m[i] ^ seed ^ (uint32_t)(i * 0x01000193u);
            if (m[i] != want) {
                /* Re-read (evict the line first): correct now = read-path error, still wrong = stored wrong. */
                Cache_Invalidate_Addr((uint32_t)(uintptr_t)&m[i], 4);
                if (m[i] == want) ++j->reread_ok;
                if (!j->errors) { j->first_off = i * 4; j->got = m[i]; j->want = want; }
                j->xor_bits |= m[i] ^ want; ++j->errors;
            }
        }
        vTaskDelay(1);
    }
    heap_caps_free(m);
}
static float chip_temp_c(void)
{
    /* ponytail: sensor installed once and left enabled; ~uA, only used by memtest */
    static temperature_sensor_handle_t t;
    float c = -99;
    if (!t) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &t) != ESP_OK || temperature_sensor_enable(t) != ESP_OK) { t = NULL; return c; }
    }
    temperature_sensor_get_celsius(t, &c);
    return c;
}
static void memtest_task(void *arg) { memtest_job_t *j = arg; memtest_run(j); xSemaphoreGive(j->done); vTaskDelete(NULL); }
/* ponytail: PSRAM read-timing probe. IDF picks one point of this table at boot from a single
 * 64-byte test (MSPI_TIMING_PSRAM_CONFIG_TABLE_CORE_CLK_160M_MODULE_CLK_80M_DTR_MODE, IDF 5.5.1)
 * and never revisits it; `cam psramtiming <idx>` lets memtest2 map the real eye per board. */
static const DRAM_ATTR uint8_t s_psram_pts[14][3] = {{0,0,0},{4,2,2},{2,1,2},{4,1,2},{1,0,1},{4,0,2},{0,0,1},{4,2,3},{2,1,3},{4,1,3},{1,0,2},{4,0,3},{0,0,2},{4,2,4}};
/* noinline: a static IRAM_ATTR function inlined into its flash caller runs from flash with the cache off. */
static void __attribute__((noinline)) IRAM_ATTR psram_timing_apply(int idx)
{
    const uint8_t mode = s_psram_pts[idx][0], num = s_psram_pts[idx][1], dummy = s_psram_pts[idx][2];
    uint32_t m = 0, n = 0;
    for (int b = 0; b < 9; ++b) { m |= (uint32_t)mode << (3 * b); n |= (uint32_t)num << (2 * b); }   /* DIN0..7 + DQS */
    spi_flash_disable_interrupts_caches_and_other_cpu();
    REG_WRITE(SPI_MEM_SPI_SMEM_DIN_MODE_REG(0), (REG_READ(SPI_MEM_SPI_SMEM_DIN_MODE_REG(0)) & ~0x07ffffffu) | m);
    REG_WRITE(SPI_MEM_SPI_SMEM_DIN_NUM_REG(0), (REG_READ(SPI_MEM_SPI_SMEM_DIN_NUM_REG(0)) & ~0x0003ffffu) | n);
    uint32_t cali = REG_READ(SPI_MEM_SPI_SMEM_TIMING_CALI_REG(0)) & ~(SPI_MEM_SPI_SMEM_TIMING_CALI_M | (SPI_MEM_SPI_SMEM_EXTRA_DUMMY_CYCLELEN_V << SPI_MEM_SPI_SMEM_EXTRA_DUMMY_CYCLELEN_S));
    if (dummy) cali |= SPI_MEM_SPI_SMEM_TIMING_CALI_M | ((uint32_t)dummy << SPI_MEM_SPI_SMEM_EXTRA_DUMMY_CYCLELEN_S);
    REG_WRITE(SPI_MEM_SPI_SMEM_TIMING_CALI_REG(0), cali);
    spi_flash_enable_interrupts_caches_and_other_cpu();
}
static int psram_timing_current(unsigned *mode, unsigned *num, unsigned *dummy)
{
    *mode = REG_READ(SPI_MEM_SPI_SMEM_DIN_MODE_REG(0)) & 7; *num = REG_READ(SPI_MEM_SPI_SMEM_DIN_NUM_REG(0)) & 3;
    uint32_t cali = REG_READ(SPI_MEM_SPI_SMEM_TIMING_CALI_REG(0));
    *dummy = (cali & SPI_MEM_SPI_SMEM_TIMING_CALI_M) ? (cali >> SPI_MEM_SPI_SMEM_EXTRA_DUMMY_CYCLELEN_S) & SPI_MEM_SPI_SMEM_EXTRA_DUMMY_CYCLELEN_V : 0;
    for (int i = 0; i < 14; ++i) if (s_psram_pts[i][0] == *mode && s_psram_pts[i][1] == *num && s_psram_pts[i][2] == *dummy) return i;
    return -1;
}

static int cmd_cam(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&cam_args);
    if (nerr != 0 || cam_args.sub->count == 0) {
        printf("usage: cam <detect|on|off|exp|orient|tune|stats|selftest|frexon|frexoff|frexexp|pattern|rate|scan|fslog|crc> [value]\n");
        return 1;
    }
    const char *sub = cam_args.sub->sval[0];

    if (ota_server_uploading() && strcmp(sub, "off") != 0 &&
        strcmp(sub, "stats") != 0) {
        printf("Error: camera controls are locked during OTA\n");
        return 1;
    }

    if (strcmp(sub, "startupvsync") == 0) {
        if (argc==2) {
            bool enabled=false;
            if (!mode_ctrl_get_startup_vsync_only(&enabled)) {
                printf("cam startupvsync read: FAILED\n");
                return 1;
            }
            printf("cam startupvsync enabled=%u volatile=1\n",(unsigned)enabled);
            return 0;
        }
        if (argc!=3 || (strcmp(argv[2],"on") && strcmp(argv[2],"off"))) return 1;
        bool ok=mode_ctrl_set_startup_vsync_only(!strcmp(argv[2],"on"));
        printf("cam startupvsync %s: %s (volatile)\n",argv[2],ok?"OK":"FAILED");
        return ok?0:1;
    }
    if(!strcmp(sub,"jpegmode")){
        if(argc==2){unsigned mode=0;uint64_t generation=0;bool ready=false;
            if(!mode_ctrl_get_jpeg_mode(&mode,&generation,&ready))return 1;
            printf("cam jpegmode requested=%u volatile=1 inherited=%u\n",mode,(unsigned)(mode==0));
            printf("jpegmode_session generation=%llu ready=%u\n",(unsigned long long)generation,(unsigned)ready);return 0;}
        if(argc!=3 || (strcmp(argv[2],"0") && strcmp(argv[2],"2") && strcmp(argv[2],"3"))){
            printf("usage: cam jpegmode [0|2|3] (OFF only;0 inherits resolution)\n");return 1;}
        unsigned mode=(unsigned)(argv[2][0]-'0');
        bool ok=mode_ctrl_set_jpeg_mode(mode);
        printf("cam jpegmode %u: %s (volatile)\n",mode,ok?"OK":"FAILED");return ok?0:1;
    }
    if(!strcmp(sub,"rawpreviewdemand")){
        if(argc==2){bool enabled=false;if(!mode_ctrl_get_raw_preview_demand(&enabled))return 1;
            printf("cam rawpreviewdemand enabled=%u volatile=1\n",(unsigned)enabled);return 0;}
        if(argc!=3 || (strcmp(argv[2],"on") && strcmp(argv[2],"off"))){
            printf("usage: cam rawpreviewdemand [on|off] (OFF only)\n");return 1;}
        bool ok=mode_ctrl_set_raw_preview_demand(!strcmp(argv[2],"on"));
        printf("cam rawpreviewdemand %s: %s (volatile)\n",argv[2],ok?"OK":"FAILED");return ok?0:1;
    }
    if(!strcmp(sub,"rawpreviewadmission")){
        if(argc==2){bool enabled=false;if(!mode_ctrl_get_raw_preview_no_replace(&enabled))return 1;
            printf("cam rawpreviewadmission enabled=%u volatile=1\n",(unsigned)enabled);return 0;}
        if(argc!=3 || (strcmp(argv[2],"on") && strcmp(argv[2],"off"))){
            printf("usage: cam rawpreviewadmission [on|off] (OFF only)\n");return 1;}
        bool ok=mode_ctrl_set_raw_preview_no_replace(!strcmp(argv[2],"on"));
        printf("cam rawpreviewadmission %s: %s (volatile)\n",argv[2],ok?"OK":"FAILED");return ok?0:1;
    }
    if(!strcmp(sub,"rawpreviewmax")){
        if(argc==2){bool enabled=false;if(!mode_ctrl_get_raw_preview_max(&enabled))return 1;
            printf("cam rawpreviewmax enabled=%u volatile=1\n",(unsigned)enabled);return 0;}
        if(argc!=3 || (strcmp(argv[2],"on") && strcmp(argv[2],"off"))){
            printf("usage: cam rawpreviewmax [on|off] (OFF only)\n");return 1;}
        bool ok=mode_ctrl_set_raw_preview_max(!strcmp(argv[2],"on"));
        printf("cam rawpreviewmax %s: %s (volatile)\n",argv[2],ok?"OK":"FAILED");return ok?0:1;
    }
    if (strcmp(sub, "cip") == 0) {
        if (argc != 3) return 1;
        camera_cip_action_t action;
        if (!strcmp(argv[2], "match")) action = CAMERA_CIP_MATCH;
        else if (!strcmp(argv[2], "restore")) action = CAMERA_CIP_RESTORE;
        else if (!strcmp(argv[2], "edgeplus1")) action = CAMERA_CIP_EDGE_PLUS_ONE;
        else if (!strcmp(argv[2], "edgereturn")) action = CAMERA_CIP_EDGE_RETURN;
        else return 1;
        return mode_ctrl_cip_command(action) ? 0 : 1;
    }
    /* Live register read/write. Bisecting a sensor fault by rebuilding and
     * flashing costs ~5 minutes per hypothesis; this makes it seconds, and
     * unlike `cam sensorregs` it reads the hardware rather than a cached
     * snapshot. Write is deliberately unrestricted: the sensor is reset by
     * the next reconfigure, so a bad poke costs one reconfigure. */
    if (strcmp(sub, "reg") == 0) {
        if (argc < 3) { printf("usage: cam reg <addr_hex> [value_hex]\\n"); return 1; }
        unsigned addr = (unsigned)strtoul(cam_args.opts->sval[0], NULL, 16);
        if (argc >= 4) {
            unsigned val = (unsigned)strtoul(cam_args.opts->sval[1], NULL, 16);
            int rc = camera_sensor_register(true, addr, val);
            printf("cam reg 0x%04x <- 0x%02x: %s\\n", addr, val, rc == 0 ? "OK" : "FAILED");
            return rc == 0 ? 0 : 1;
        }
        int got = camera_sensor_register(false, addr, 0);
        if (got < 0) { printf("cam reg 0x%04x: read failed\\n", addr); return 1; }
        printf("cam reg 0x%04x = 0x%02x\\n", addr, got);
        return 0;
    }
    if (strcmp(sub, "sensorregs") == 0) {
        if (argc != 2) { printf("usage: cam sensorregs (initialized camera only)\n"); return 1; }
        bool ok = mode_ctrl_print_sensor_registers();
        printf("cam sensorregs: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub,"deferredeof")==0 || strcmp(sub,"defercontent")==0) {
        const char *value=cam_args.opts->count ? cam_args.opts->sval[0] : "";
        unsigned wait_us;
        if (argc!=3) { printf("usage: cam <deferredeof|defercontent> <0|100|500|2000> (OFF only)\n"); return 1; }
        if (!strcmp(value,"0")) wait_us=0;
        else if (!strcmp(value,"100")) wait_us=100;
        else if (!strcmp(value,"500")) wait_us=500;
        else if (!strcmp(value,"2000")) wait_us=2000;
        else { printf("invalid deferred EOF observation bound\n"); return 1; }
        return (strcmp(sub,"defercontent")==0 ? mode_ctrl_run_deferred_content(wait_us) : mode_ctrl_run_deferred_eof(wait_us)) ? 0 : 1;
    }
    if (strcmp(sub, "rawtrace") == 0) {
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        if (argc != 3 || (strcmp(value, "arm") != 0 && strcmp(value, "off") != 0 && strcmp(value, "read") != 0)) {
            printf("usage: cam rawtrace <arm|off|read> (OFF with camera deinitialized)\n");
            return 1;
        }
        bool ok = strcmp(value, "read") == 0 ? mode_ctrl_print_raw_eof_trace() :
                  mode_ctrl_set_raw_eof_trace(strcmp(value, "arm") == 0);
        printf("rawtrace %s: %s\n", value, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "pattern") == 0) {
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        if (strcmp(value, "on") != 0 && strcmp(value, "off") != 0 &&
            strcmp(value, "squares") != 0) {
            printf("usage: cam pattern <on|off|squares> (active camera only, not persisted)\n");
            return 1;
        }
        mode_test_pattern_t pattern = strcmp(value, "squares") == 0
            ? MODE_TEST_PATTERN_STATIONARY_COLOR_SQUARES
            : strcmp(value, "on") == 0 ? MODE_TEST_PATTERN_ON : MODE_TEST_PATTERN_OFF;
        bool ok = mode_ctrl_set_test_pattern(pattern);
        printf("sensor test pattern %s: %s\n", value, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "rawexp") == 0) {
        /* Real AEC row count (set_aec_value), not the AE bias level that
         * `cam tune set exposure` goes through - that path calls
         * set_ae_level (a -5..+5 bias), so a row count sent there is
         * clamped and silently ignored.
         *
         * Handled BEFORE the perf gate: this is a live register write,
         * serialised by camera_sensor_mutation_begin(), and it must work
         * while capture runs. Reading a phone screen needs the exposure
         * retuned in flight, and nodes cannot be stopped to do it without
         * losing the synchronized session.
         *
         * Measured 2026-09-17: this sets exposure_rows (885 -> 400 confirmed)
         * but does NOT shorten the frame period. VTS is a fixed 984 from the
         * driver's VGA table, not derived from exposure, and live_track
         * re-applies auto exposure when its loop starts. */
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        unsigned rows = 0; char extra;
        bool ok = sscanf(value, "%u%c", &rows, &extra) == 1 && rows <= 4095 &&
                  camera_set_tracking_exposure((uint16_t)rows);
        printf("cam rawexp %s: %s\n", value, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "rawgain") == 0) {
        /* Live AGC gain, the companion to rawexp. Exposing for a bright
         * subject needs both: with gain pinned high for IR markers the
         * sensor saturates whatever the exposure. Ungated for the same
         * reason - it is a register write, not a pipeline change. */
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        unsigned gain = 0; char extra;
        bool ok = sscanf(value, "%u%c", &gain, &extra) == 1 && gain <= 64 &&
                  camera_set_tracking_gain((uint8_t)gain);
        printf("cam rawgain %s: %s\n", value, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "rawquality") == 0) {
        /* Live JPEG quality (0-63, higher = more compression on OV5640).
         *
         * Needed because one node's sensor is measurably noisier than its
         * peers: on the sensor's OWN test pattern - an identical synthetic
         * image - node1 emits ~100 kB where every other node emits ~27 kB.
         * Noise is incompressible, so that node saturates the 1 MB/s budget
         * and its send pool evicts the backlog (replaced=517 vs 0), leaving
         * it at 17-25 delivered frames against ~59 for the others.
         *
         * Exposure cannot fix this and neither can the transport; the only
         * lever that shrinks an incompressible frame is compression itself.
         * Ungated like the other live sensor writes. */
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        unsigned quality = 0; char extra;
        bool ok = sscanf(value, "%u%c", &quality, &extra) == 1 &&
                  quality <= 63 && camera_set_jpeg_quality((uint8_t)quality);
        printf("cam rawquality %s: %s\n", value, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "memtest") == 0 || strcmp(sub, "memtest2") == 0) {
        /* ponytail: PSRAM pattern test through the cache - buffer >> 64 KiB dcache, so every
         * line is written back and re-read from the chip. memtest2 also loads the other core. */
        unsigned kib = 1024; char extra;
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        if (*value && sscanf(value, "%u%c", &kib, &extra) != 1) kib = 0;
        if (kib < 128 || kib > 4096) { printf("usage: cam memtest|memtest2 <128..4096 KiB>\n"); return 1; }
        memtest_job_t me = { .words = (size_t)kib * 256, .passes = 4 }, other = me;
        TaskHandle_t helper = NULL;
        if (strcmp(sub, "memtest2") == 0) {
            other.done = xSemaphoreCreateBinary();
            if (!other.done || xTaskCreatePinnedToCore(memtest_task, "memtest", 3072, &other, 5, &helper, !xPortGetCoreID()) != pdPASS) {
                printf("memtest: helper start failed\n"); return 1;
            }
        }
        memtest_run(&me);
        if (helper) { xSemaphoreTake(other.done, portMAX_DELAY); vSemaphoreDelete(other.done); }
        uint8_t mac[6] = {0}; esp_chip_info_t chip; esp_efuse_mac_get_default(mac); esp_chip_info(&chip);
        printf("memtest: temp_c=%.1f kib=%u passes=%u errors=%u other_core_errors=%u reread_ok=%u first_off=0x%x got=0x%08lx want=0x%08lx xor_bits=0x%08lx"
               " mac=%02x%02x%02x%02x%02x%02x chip_rev=%u psram_kib=%u reset_reason=%d uptime_s=%llu\n",
               chip_temp_c(), kib, me.passes, me.errors, other.errors, me.reread_ok + other.reread_ok, me.first_off, (unsigned long)me.got,
               (unsigned long)me.want, (unsigned long)(me.xor_bits | other.xor_bits),
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)chip.revision, (unsigned)(esp_psram_get_size() / 1024), (int)esp_reset_reason(),
               (unsigned long long)(esp_timer_get_time() / 1000000));
        return (me.errors || other.errors || me.failed || other.failed) ? 1 : 0;
    }
    if (strcmp(sub, "psramtiming") == 0) {
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        unsigned idx = 0; char extra;
        if (*value) {
            if (sscanf(value, "%u%c", &idx, &extra) != 1 || idx > 13) { printf("usage: cam psramtiming [0..13]\n"); return 1; }
            psram_timing_apply((int)idx);
        }
        unsigned mode, num, dummy; int cur = psram_timing_current(&mode, &num, &dummy);
        printf("psramtiming: idx=%d din_mode=%u din_num=%u extra_dummy=%u\n", cur, mode, num, dummy);
        return 0;
    }
    if (strcmp(sub, "hwstamp") == 0 || strcmp(sub, "hwtrig") == 0) {
        /* Capture-path timing A/B, safe while capture runs:
         *   hwstamp: frame timestamp = MCPWM-latched VSYNC edge, not cam_task.
         *   hwtrig:  FREX request write started by a GPTimer alarm at the slot.
         * `stats` prints and resets the counters. */
        const bool stamp = sub[2] == 's';
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        const bool on = strcmp(value, "on") == 0, off = strcmp(value, "off") == 0;
        if (on || off) {
            bool ok = true;
            if (stamp) ok = vsync_stamp_set(on); else frex_sync_hw_set(on);
            printf("cam %s %s: %s\n", sub, value, ok ? "OK" : "FAILED");
            return ok ? 0 : 1;
        }
        if (strcmp(value, "stats") != 0) {
            printf("usage: cam %s <on|off|stats>\n", sub);
            return 1;
        }
        if (stamp) {
            vsync_stamp_stats_t s;
            vsync_stamp_stats(&s, true);
            printf("hwstamp on=%d hits=%lu misses=%lu rise=%lu fall=%lu sw_lag_us=%ld..%ld mean=%lld isr_lag_us=%ld..%ld\n",
                   s.on, (unsigned long)s.hits, (unsigned long)s.misses, (unsigned long)s.rise,
                   (unsigned long)s.fall, (long)s.sw_lag_min_us, (long)s.sw_lag_max_us,
                   (long long)(s.hits ? s.sw_lag_sum_us / s.hits : 0),
                   (long)s.isr_lag_min_us, (long)s.isr_lag_max_us);
        } else {
            frex_hw_stats_t s;
            frex_sync_hw_stats(&s, true);
            printf("hwtrig on=%d ok=%lu late=%lu nack=%lu timeout=%lu busy=%lu fire_us=%ld..%ld done_us=%ld..%ld\n",
                   frex_sync_hw_get(), (unsigned long)s.ok, (unsigned long)s.late, (unsigned long)s.nack,
                   (unsigned long)s.timeout, (unsigned long)s.busy, (long)s.fire_min_us,
                   (long)s.fire_max_us, (long)s.done_min_us, (long)s.done_max_us);
        }
        return 0;
    }
    if (strcmp(sub, "syncfrex") == 0) {
        /* Selects the synchronized-capture shutter mode. Read only when
         * sync_capture starts, so it is safe to set while a preview runs and
         * is deliberately NOT behind the STROBE_OFF perf guard -- the whole
         * point is to flip modes between back-to-back comparison runs. */
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        bool on = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
        bool off = strcmp(value, "off") == 0 || strcmp(value, "0") == 0;
        if (!on && !off) {
            printf("usage: cam syncfrex <on|off>  (now: %s)\n",
                   sync_capture_frex_enabled() ? "on" : "off");
            return 1;
        }
        bool ok = sync_capture_set_frex_enabled(on);
        printf("cam syncfrex %s: %s\n", on ? "on" : "off", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    /* Settings that reconfigure buffers, clocks or DMA cannot change under a
     * running pipeline, so they stay gated on STROBE_OFF. `rawexp` is NOT one
     * of them: it is a live sensor register write already serialised by
     * camera_sensor_mutation_begin(), which exists precisely so exposure can
     * be retuned in flight. Gating it here made every in-capture exposure
     * change a silent no-op - a sweep from 400 down to 25 moved measured
     * brightness by under one count because nothing was ever applied. */
    if (strcmp(sub, "perf") == 0 || strcmp(sub, "tx") == 0 || strcmp(sub, "txpacer") == 0 || strcmp(sub, "txretry") == 0 || strcmp(sub, "diag") == 0 || strcmp(sub, "txbackoff") == 0 || strcmp(sub, "algo") == 0 || strcmp(sub, "jpegclock") == 0 || strcmp(sub, "rawclock") == 0 || strcmp(sub, "rawbuffers") == 0 || strcmp(sub, "qdiv") == 0 || strcmp(sub, "jpegbuffers") == 0 || strcmp(sub, "jpeghts") == 0 || strcmp(sub, "rawdma") == 0 || strcmp(sub, "hirate") == 0 || strcmp(sub, "rawhts") == 0 || strcmp(sub, "minarea") == 0 || strcmp(sub, "border") == 0 || strcmp(sub, "rawring") == 0 || strcmp(sub, "syncbufs") == 0 || strcmp(sub, "rawchunk") == 0 || strcmp(sub, "radiopower") == 0 || strcmp(sub, "copyprio") == 0 || strcmp(sub, "rowstream") == 0) {
        if (strobe_gpio_get_mode() != STROBE_OFF) { printf("Error: stop capture before tuning performance\n"); return 1; }
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        bool ok = false;
        if (strcmp(sub, "perf") == 0) {
            ok = strcmp(value, "on") == 0 || strcmp(value, "off") == 0;
            if (ok) ok = camera_set_perf_profile(strcmp(value, "on") == 0);
        } else if (strcmp(sub, "diag") == 0) {
            ok = strcmp(value, "on") == 0 || strcmp(value, "off") == 0;
            if (ok) ok = camera_set_pipeline_diagnostics(strcmp(value, "on") == 0);
        } else if (strcmp(sub, "copyprio") == 0) {
            unsigned prio=0; char extra;
            ok = sscanf(value, "%u%c", &prio, &extra) == 1 && camera_set_copy_prio(prio);
        } else if (strcmp(sub, "rawdma") == 0) {
            ok = strcmp(value, "on") == 0 || strcmp(value, "off") == 0;
            if (ok) ok = camera_set_raw_direct_dma(strcmp(value, "on") == 0);
        } else if (strcmp(sub, "radiopower") == 0) {
            unsigned power=0; char extra; int8_t effective=0;
            ok = sscanf(value, "%u%c", &power, &extra) == 1 && power >= 8 && power <= 84;
            if (ok) ok = esp_wifi_set_max_tx_power((int8_t)power) == ESP_OK &&
                         esp_wifi_get_max_tx_power(&effective) == ESP_OK;
            if (ok) printf("radio power: requested_quarter_dbm=%u effective_quarter_dbm=%d\n", power, (int)effective);
        } else if (strcmp(sub, "rawchunk") == 0) {
            /* Exact choices avoid accepting signs, overflow or trailing text. */
            if (cam_args.opts->count == 1) {
                if (strcmp(value, "0") == 0) ok = camera_set_raw_dma_chunk(0);
                else if (strcmp(value, "7680") == 0) ok = camera_set_raw_dma_chunk(7680);
                else if (strcmp(value, "3840") == 0) ok = camera_set_raw_dma_chunk(3840);
            }
        } else if (strcmp(sub, "rawring") == 0) {
            unsigned kib=0; char extra;
            ok = sscanf(value, "%u%c", &kib, &extra) == 1 && camera_set_raw_dma_ring(kib);
        } else if (strcmp(sub, "syncbufs") == 0) {
            unsigned n=0; char extra;
            ok = sscanf(value, "%u%c", &n, &extra) == 1 && camera_set_raw_sync_buffers(n);
        } else if (strcmp(sub, "qdiv") == 0) {
            unsigned divisor=0; char extra;
            ok = sscanf(value, "%u%c", &divisor, &extra) == 1 && camera_set_qvga_pclk_div(divisor);
        } else if (strcmp(sub, "jpegbuffers") == 0) {
            unsigned count=0; char extra;
            ok = sscanf(value, "%u%c", &count, &extra) == 1 && camera_set_jpeg_buffer_count(count);
        } else if (strcmp(sub, "rawbuffers") == 0) {
            unsigned count=0; char extra;
            ok = sscanf(value, "%u%c", &count, &extra) == 1 && camera_set_raw_buffer_count(count);
        } else if (strcmp(sub, "rawclock") == 0) {
            unsigned mhz=0; char extra;
            ok = sscanf(value, "%u%c", &mhz, &extra) == 1 && camera_set_tracking_vco(mhz);
        } else if (strcmp(sub, "border") == 0) {
            /* Edge margin; rejects vignette/readout corner artifacts. */
            unsigned px=0; char extra;
            ok = sscanf(value, "%u%c", &px, &extra) == 1 && mode_ctrl_set_border(px);
        } else if (strcmp(sub, "minarea") == 0) {
            /* Smallest accepted blob; the IR marker is often 2-3 px. */
            unsigned area=0; char extra;
            ok = sscanf(value, "%u%c", &area, &extra) == 1 && mode_ctrl_set_min_area(area);
        } else if (strcmp(sub, "rawhts") == 0) {
            /* Tracking line length: 2060 stock (IR-friendly), 1556 fast. */
            unsigned hts=0; char extra;
            ok = sscanf(value, "%u%c", &hts, &extra) == 1 && camera_set_tracking_hts(hts);
        } else if (strcmp(sub, "hirate") == 0) {
            /* QVGA high-rate stream profile; off by default because it
             * yields an all-black JPEG on these OV5640 modules. */
            bool on = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
            bool off = strcmp(value, "off") == 0 || strcmp(value, "0") == 0;
            ok = (on || off) && sync_capture_set_high_rate_qvga(on);
        } else if (strcmp(sub, "jpeghts") == 0) {
            unsigned hts=0; char extra;
            ok = sscanf(value, "%u%c", &hts, &extra) == 1 && camera_set_preview_hts(hts);
        } else if (strcmp(sub, "jpegclock") == 0) {
            unsigned mhz=0; char extra;
            ok = sscanf(value, "%u%c", &mhz, &extra) == 1 && camera_set_preview_vco(mhz);
        } else if (strcmp(sub, "txpacer") == 0 || strcmp(sub, "txretry") == 0 || strcmp(sub, "txbackoff") == 0) {
            unsigned setting=0; char extra;
            ok = sscanf(value, "%u%c", &setting, &extra) == 1;
            if (ok) {
                if (strcmp(sub, "txpacer") == 0) ok = camera_udp_set_pacer(setting);
                else if (strcmp(sub, "txbackoff") == 0) ok = camera_udp_set_backoff_cap(setting);
                else ok = camera_udp_set_retry_limit(setting);
            }
        } else if (strcmp(sub, "tx") == 0) {
            unsigned kbps=0, burst=0; char extra;
            ok = sscanf(value, "%u,%u%c", &kbps, &burst, &extra) == 2 && camera_udp_set_pacing(kbps, burst);
        } else if (strcmp(sub, "rowstream") == 0) {
            unsigned mode = strcmp(value, "off") == 0 ? 0 : strcmp(value, "on") == 0 ? 1 :
                            strcmp(value, "verify") == 0 ? 2 : 3;
            ok = mode <= 2;
            if (ok) live_track_set_row_stream(mode);
        } else {
            unsigned algorithm=0; char extra;
            ok = sscanf(value, "%u%c", &algorithm, &extra) == 1 && centroid_detector_set_algorithm(algorithm);
        }
        printf("cam %s %s: %s (volatile)\n", sub, value, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "detect") == 0) {
        cam_detect_result_t r = camera_detect();
        printf("detect: ok=%d sensor=%s pid=0x%04x preset=%d (%s)\n",
               r.ok, r.sensor_name, r.pid, r.preset, r.preset_name);
        return r.ok ? 0 : 1;
    }
    if (strcmp(sub, "orient") == 0) {
        if (cam_args.opts->count != 2) {
            printf("usage: cam orient <hmirror:0|1> <vflip:0|1>\n");
            return 1;
        }
        char *end_h = NULL;
        char *end_v = NULL;
        long hmirror = strtol(cam_args.opts->sval[0], &end_h, 10);
        long vflip = strtol(cam_args.opts->sval[1], &end_v, 10);
        if (!end_h || *end_h != '\0' || !end_v || *end_v != '\0' ||
            (hmirror != 0 && hmirror != 1) || (vflip != 0 && vflip != 1)) {
            printf("usage: cam orient <hmirror:0|1> <vflip:0|1>\n");
            return 1;
        }
        bool ok = camera_set_orientation_override(hmirror != 0, vflip != 0);
        printf("cam orient hmirror=%ld vflip=%ld: %s\n",
               hmirror, vflip, ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "on") == 0) {
        cam_res_t res = CAM_RES_VGA;
        bool raw_gray = true;
        for (int i = 0; i < cam_args.opts->count; i++) {
            const char *opt = cam_args.opts->sval[i];
            if (strcmp(opt, "gray") == 0) {
                raw_gray = true;
            } else if (strcmp(opt, "jpeg") == 0) {
                printf("TCP JPEG is disabled; use USB camera mode for JPEG\n");
                return 1;
            } else if (strcmp(opt, "sxga") == 0) {
                res = CAM_RES_SXGA;
            } else if (strcmp(opt, "hd") == 0) {
                res = CAM_RES_HD;
            } else if (strcmp(opt, "fhd") == 0) {
                res = CAM_RES_FHD;
            } else if (strcmp(opt, "qvga") == 0) {
                res = CAM_RES_QVGA;
            } else if (strcmp(opt, "vga") != 0) {
                printf("usage: cam on [qvga|vga|hd|fhd|sxga] [gray]\n");
                return 1;
            }
        }
        (void)raw_gray;
        bool ok = mode_ctrl_start_tcp_preview(res);
        const char *res_name = res == CAM_RES_QVGA ? "qvga" :
                               res == CAM_RES_SXGA ? "sxga" :
                               res == CAM_RES_HD ? "hd" :
                               res == CAM_RES_FHD ? "fhd" : "vga";
        printf("cam on %s gray (TCP): %s\n", res_name,
               ok ? "started" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "off") == 0) {
        bool ok = mode_ctrl_apply(STROBE_OFF, 0);
        printf("cam off: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "exp") == 0) {
        int level = 0;
        bool have_level = false;
        if (cam_args.a1i->count > 0) {
            level = cam_args.a1i->ival[0];
            have_level = true;
        } else if (cam_args.opts->count > 0) {
            char *end = NULL;
            long parsed = strtol(cam_args.opts->sval[0], &end, 10);
            if (end && *end == '\0') {
                level = (int)parsed;
                have_level = true;
            }
        }
        if (!have_level) {
            printf("usage: cam exp <level>   (-1 = auto)\n");
            return 1;
        }
        bool ok = camera_set_exposure(level);
        printf("camera exposure: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "tune") == 0) {
        camera_tuning_t tuning;
        camera_get_tuning(&tuning);
        const char *action = cam_args.opts->count > 0 ? cam_args.opts->sval[0] : "get";
        if (strcmp(action, "set") == 0) {
            if (cam_args.opts->count < 2) {
                printf("usage: cam tune set <quality,exposure,gain,wb,brightness,contrast,saturation,sharpness>\n");
                return 1;
            }
            int q, exposure, gain, wb, brightness, contrast, saturation, sharpness;
            if (sscanf(cam_args.opts->sval[1], "%d,%d,%d,%d,%d,%d,%d,%d",
                       &q, &exposure, &gain, &wb, &brightness, &contrast,
                       &saturation, &sharpness) != 8) {
                printf("Error: tuning profile must contain eight comma-separated integers\n");
                return 1;
            }
            tuning.jpeg_quality = (uint8_t)q;
            tuning.exposure_lines = (int16_t)exposure;
            tuning.gain = (int8_t)gain;
            tuning.white_balance = (uint8_t)wb;
            tuning.brightness = (int8_t)brightness;
            tuning.contrast = (int8_t)contrast;
            tuning.saturation = (int8_t)saturation;
            tuning.sharpness = (int8_t)sharpness;
            if (q < 0 || q > 63 || !camera_set_tuning(&tuning, true)) {
                printf("Error: invalid or unsupported camera tuning profile\n");
                return 1;
            }
        } else if (strcmp(action, "get") != 0) {
            printf("usage: cam tune <get|set profile>\n");
            return 1;
        }
        camera_get_tuning(&tuning);
        printf("tune: quality=%u exposure=%d gain=%d wb=%u brightness=%d contrast=%d saturation=%d sharpness=%d\n",
               tuning.jpeg_quality, tuning.exposure_lines, tuning.gain,
               tuning.white_balance, tuning.brightness, tuning.contrast,
               tuning.saturation, tuning.sharpness);
        /* Requested values read back as -1 under AEC/AGC, so also report what
         * the sensor actually resolved. */
        {
            uint32_t live_rows = 0; uint16_t live_gain = 0, live_hts = 0, live_vts = 0;
            if (camera_read_live_exposure(&live_rows, &live_gain, &live_hts, &live_vts)) {
                /* row_period_ns lets the host express exposure as time rather
                 * than rows: exposure_us = exposure_rows * row_period_ns/1000.
                 * Sourced from camera_row_period_ns() so it tracks the live
                 * HTS/SCLK instead of a hardcoded assumption. */
                printf("live: exposure_rows=%u gain_x16=%u gain=%.2fx hts=%u vts=%u row_period_ns=%u\n",
                       (unsigned)live_rows, (unsigned)live_gain,
                       (double)live_gain / 16.0, (unsigned)live_hts, (unsigned)live_vts,
                       (unsigned)camera_row_period_ns());
            } else {
                printf("live: unavailable (camera not initialised)\n");
            }
        }
        return 0;
    }
    if (strcmp(sub, "stats") == 0) {
        camera_print_profile();
        /* Frame buffers live in PSRAM, so a board whose PSRAM is smaller or
         * misdetected fails only the large allocations (Y8 VGA is 307 KB while
         * Y8 QVGA is 77 KB). Report the largest free block so an ESP_FAIL from
         * camera init can be attributed instead of guessed at. */
        printf("heap: psram_free=%u psram_largest=%u internal_free=%u internal_largest=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        int8_t power=-1;
        wifi_ap_record_t ap;
        esp_wifi_get_max_tx_power(&power);
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
        printf("radio profile: txpower_quarter_dbm=%d rssi_dbm=%d boot_quarter_dbm=%d phy_reduce_after_brownout=%d\n",
               (int)power, rssi, CONFIG_FTMCS_WIFI_BOOT_TX_POWER,
#if CONFIG_ESP_PHY_REDUCE_TX_POWER
               1
#else
               0
#endif
        );
        camera_udp_print_stats();
        uvc_webcam_print_stats();
        live_track_print_stats();
        sync_capture_stats_t ss;
        sync_capture_get_stats(&ss);
        printf("sync: running=%u udp=%u frex=%u period_us=%lu frames=%lu "
               "grabs=%lu grab_fail=%lu stale=%lu recover=%lu/%lu clock_fail=%lu clock_gen=%lu "
               "wait_bad=%lu trigger_fail=%lu trigger_slots=%lu tx_drop=%lu tx_sent=%lu tx_replaced=%lu "
               "tx_pending=%lu tx_inflight=%lu last_latency=%ld epoch_rejects=%lu\n",
               ss.running, ss.udp, ss.frex, (unsigned long)ss.period_us,
               (unsigned long)ss.frames_published,
               (unsigned long)ss.grab_attempts,
               (unsigned long)ss.grab_failures,
               (unsigned long)ss.stale_rejects,
               (unsigned long)ss.recoveries,
               (unsigned long)ss.recovery_failures,
               (unsigned long)ss.clock_failures,
               (unsigned long)ss.clock_generation,
               (unsigned long)ss.invalid_waits,
               (unsigned long)ss.trigger_failures,
               (unsigned long)ss.trigger_slots,
               (unsigned long)ss.transport_drops,
               (unsigned long)ss.transport_sent,
               (unsigned long)ss.transport_replaced,
               (unsigned long)ss.transport_pending,
               (unsigned long)ss.transport_inflight,
               (long)ss.last_latency_us,
               (unsigned long)ss.epoch_rejects);
        return 0;
    }
    if (strcmp(sub, "fslog") == 0) {
        const char *value = cam_args.opts->count > 0
            ? cam_args.opts->sval[0] : NULL;
        if (value && strcmp(value, "on") == 0) {
            sync_capture_set_frame_log(true);
        } else if (value && strcmp(value, "off") == 0) {
            sync_capture_set_frame_log(false);
        } else if (value) {
            printf("usage: cam fslog <on|off>\n");
            return 1;
        }
        printf("cam fslog: %s\n",
               sync_capture_frame_log_enabled() ? "on" : "off");
        return 0;
    }
    if (strcmp(sub, "crc") == 0) {
        const char *value = cam_args.opts->count > 0
            ? cam_args.opts->sval[0] : NULL;
        if (value && strcmp(value, "on") == 0) {
            uvc_webcam_set_crc_diagnostics(true);
        } else if (value && strcmp(value, "off") == 0) {
            uvc_webcam_set_crc_diagnostics(false);
        } else if (value) {
            printf("usage: cam crc <on|off>\n");
            return 1;
        }
        printf("cam crc: %s\n",
               uvc_webcam_crc_diagnostics_enabled() ? "on" : "off");
        return 0;
    }
    if (strcmp(sub, "framecheck") == 0) {
        const char *value = cam_args.opts->count ? cam_args.opts->sval[0] : "";
        cam_res_t res;
        if (strcmp(value, "qvga") == 0) res = CAM_RES_QVGA;
        else if (strcmp(value, "vga") == 0) res = CAM_RES_VGA;
        else if (strcmp(value, "hd") == 0) res = CAM_RES_HD;
        else if (strcmp(value, "sxga") == 0) res = CAM_RES_SXGA;
        else if (strcmp(value, "fhd") == 0) res = CAM_RES_FHD;
        else { printf("usage: cam framecheck <qvga|vga|hd|sxga|fhd>\n"); return 1; }
        bool ok = mode_ctrl_run_raw_pattern_check(res);
        printf("raw pattern selftest: %s\n", ok ? "PASS" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "selftest") == 0) {
        if (live_track_running()) {
            printf("Error: stop live tracking before running the detector self-test\n");
            return 1;
        }
        bool ok = mode_ctrl_run_centroid_selftest();
        printf("centroid selftest: %s\n", ok ? "PASS" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "frexexp") == 0) {
        /* Re-arm FREX with an explicit exposure, WITHOUT touching the rolling
         * shutter registers (0x3500-02) that `cam exp` drives.
         *
         * This is the decisive test of whether FREX actually governs the
         * delivered image. Both exposures are normally programmed to the same
         * value, so they cannot be told apart. Drive them to different values
         * and the brightness reveals which one the sensor obeyed:
         *   brightness tracks this value  -> FREX owns the shutter
         *   brightness ignores this value -> the frame is a rolling-shutter
         *                                    readout and sync is cosmetic */
        if (cam_args.opts->count == 0) {
            printf("usage: cam frexexp <tlines>\n");
            return 1;
        }
        char *end = NULL;
        long lines = strtol(cam_args.opts->sval[0], &end, 10);
        if (!end || *end != '\0' || lines < 1 || lines > 65535) {
            printf("Error: FREX exposure must be 1..65535 Tlines\n");
            return 1;
        }
        if (!frex_sync_active()) {
            printf("Error: FREX not enabled; start a synchronized mode first\n");
            return 1;
        }
        if (!frex_sync_enable((uint16_t)lines)) {
            printf("Error: FREX re-arm failed\n");
            return 1;
        }
        printf("frex exposure set: %ld Tlines (rolling shutter untouched)\n", lines);
        return 0;
    }
    if (strcmp(sub, "frexoff") == 0 || strcmp(sub, "frexon") == 0) {
        /* Toggle FREX WITHOUT restarting capture, so an A/B test changes
         * exactly one variable.
         *
         * Every previous attempt to prove FREX compared different capture
         * sessions (synchronized vs free-running), which also changes the
         * trigger cadence, the grab path and the sensor restart. Those
         * confound the result. Here the capture loop keeps running and only
         * the frame-exposure mode changes, so any difference in motion blur
         * is attributable to FREX alone. */
        bool want_on = (strcmp(sub, "frexon") == 0);
        if (!sync_capture_running()) {
            printf("Error: start a synchronized capture first\n");
            return 1;
        }
        if (want_on) {
            camera_tuning_t t;
            camera_get_tuning(&t);
            uint16_t lines = (t.exposure_lines > 0) ? (uint16_t)t.exposure_lines : 128;
            if (!frex_sync_enable(lines)) {
                printf("frex: enable FAILED\n");
                return 1;
            }
            printf("frex: ENABLED (%u Tlines)\n", lines);
        } else {
            frex_sync_disable();
            printf("frex: DISABLED (sensor free-runs; triggers become no-ops)\n");
        }
        return 0;
    }
    if (strcmp(sub, "rate") == 0) {
        uint32_t requested_mhz = 0;
        bool have_rate = false;
        if (cam_args.a1i->count > 0) {
            requested_mhz = (uint32_t)cam_args.a1i->ival[0];
            have_rate = true;
        } else if (cam_args.opts->count > 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(cam_args.opts->sval[0], &end, 10);
            if (end && *end == '\0') {
                requested_mhz = (uint32_t)parsed;
                have_rate = true;
            }
        }
        if (!have_rate) {
            printf("usage: cam rate <12..40 MHz>  (native-Y8 QVGA only)\n");
            return 1;
        }
        uint32_t actual_mhz = 0;
        bool ok = mode_ctrl_set_tracking_pclk(requested_mhz, &actual_mhz);
        printf("cam rate: %s actual=%lu MHz\n", ok ? "ok" : "FAILED",
               (unsigned long)actual_mhz);
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "scan") == 0) {
        const char *value = cam_args.opts->count > 0
            ? cam_args.opts->sval[0] : NULL;
        int factor = value ? atoi(value) : 0;
        if (factor != 2) {
            printf("usage: cam scan 2  (4x changes geometry and needs its own intrinsic profile)\n");
            return 1;
        }
        bool ok = mode_ctrl_set_tracking_subsample((uint8_t)factor);
        printf("cam scan: %s factor=%d\n", ok ? "ok" : "FAILED", factor);
        return ok ? 0 : 1;
    }
    printf("unknown cam subcommand: %s\n", sub);
    return 1;
}

/* ---- intrinsic command ---- */
static struct {
    struct arg_str *sub;
    struct arg_str *res;
    struct arg_str *K;     /* "fx,0,cx,0,fy,cy,0,0,1" */
    struct arg_str *D;     /* "k1,k2,p1,p2,k3,k4,k5,k6" */
    struct arg_int *w;
    struct arg_int *h;
    struct arg_int *pid;
    struct arg_dbl *reproj;
    struct arg_int *hmirror;
    struct arg_int *vflip;
    struct arg_end *end;
} intr_args;

static cam_res_t parse_res(const char *s)
{
    if (s && strcmp(s, "qvga") == 0) return CAM_RES_QVGA;
    if (s && strcmp(s, "sxga") == 0) return CAM_RES_SXGA;
    if (s && strcmp(s, "hd") == 0) return CAM_RES_HD;
    if (s && strcmp(s, "fhd") == 0) return CAM_RES_FHD;
    return CAM_RES_VGA;
}

static bool parse_res_strict(const char *s, cam_res_t *out)
{
    if (!s || !out) return false;
    if (strcmp(s, "qvga") == 0) *out = CAM_RES_QVGA;
    else if (strcmp(s, "vga") == 0) *out = CAM_RES_VGA;
    else if (strcmp(s, "sxga") == 0) *out = CAM_RES_SXGA;
    else if (strcmp(s, "hd") == 0) *out = CAM_RES_HD;
    else if (strcmp(s, "fhd") == 0) *out = CAM_RES_FHD;
    else return false;
    return true;
}

/* parse "a,b,c,..." into double[]; returns count or -1 */
static int parse_doubles(const char *s, double *out, int max)
{
    int n = 0;
    char buf[256];
    if (!s || strlen(s) >= sizeof(buf)) return -1;
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    while (tok && n < max) {
        char *end = NULL;
        errno = 0;
        double value = strtod(tok, &end);
        if (errno != 0 || !end || *end != '\0' || !isfinite(value)) return -1;
        out[n++] = value;
        tok = strtok_r(NULL, ",", &save);
    }
    if (tok) return -1;
    return n;
}

static int cmd_intrinsic(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&intr_args);
    if (nerr != 0 || intr_args.sub->count == 0) {
        printf("usage: intrinsic <get|clear|set ...>\n");
        return 1;
    }
    const char *sub = intr_args.sub->sval[0];

    if (strcmp(sub, "get") == 0) {
        cam_res_t r = CAM_RES_VGA;
        if (intr_args.res->count && !parse_res_strict(intr_args.res->sval[0], &r)) {
            printf("Error: unknown intrinsic resolution\n");
            return 1;
        }
        intrinsic_print(r);
        /* also print the other one if no res was given */
        if (!intr_args.res->count) {
            intrinsic_print(CAM_RES_QVGA);
            intrinsic_print(CAM_RES_SXGA);
            intrinsic_print(CAM_RES_HD);
            intrinsic_print(CAM_RES_FHD);
        }
        return 0;
    }
    if (strcmp(sub, "clear") == 0) {
        cam_res_t r = CAM_RES_VGA;
        if (intr_args.res->count && !parse_res_strict(intr_args.res->sval[0], &r)) {
            printf("Error: unknown intrinsic resolution\n");
            return 1;
        }
        bool ok = intrinsic_clear(r);
        printf("intrinsic clear: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "set") == 0) {
        if (intr_args.K->count == 0 || intr_args.D->count == 0 ||
            intr_args.w->count == 0 || intr_args.h->count == 0 ||
            intr_args.pid->count == 0) {
            printf("usage: intrinsic set <qvga|vga|hd|fhd|sxga> <K:9csv> <D:8csv> "
                   "<w> <h> <pid> [reproj] [hmirror] [vflip]\n");
            return 1;
        }
        cam_res_t r;
        if (intr_args.res->count == 0 ||
            !parse_res_strict(intr_args.res->sval[0], &r)) {
            printf("Error: unknown intrinsic resolution\n");
            return 1;
        }
        double K[9] = {0}, D[8] = {0};
        int nk = parse_doubles(intr_args.K->sval[0], K, 9);
        int nd = parse_doubles(intr_args.D->sval[0], D, 8);
        if (nk != 9) {
            printf("K must have 9 values (got %d)\n", nk);
            return 1;
        }
        if (nd != 8) {
            printf("D must have 8 values (got %d)\n", nd);
            return 1;
        }
        double reproj = intr_args.reproj->count ? intr_args.reproj->dval[0] : 0.0;
        int hmirror_value = intr_args.hmirror->count ? intr_args.hmirror->ival[0] : 1;
        int vflip_value = intr_args.vflip->count ? intr_args.vflip->ival[0] : 1;
        if ((hmirror_value != 0 && hmirror_value != 1) ||
            (vflip_value != 0 && vflip_value != 1) ||
            intr_args.pid->ival[0] <= 0 || intr_args.pid->ival[0] > UINT16_MAX ||
            intr_args.w->ival[0] <= 0 || intr_args.w->ival[0] > UINT16_MAX ||
            intr_args.h->ival[0] <= 0 || intr_args.h->ival[0] > UINT16_MAX) {
            printf("Error: invalid intrinsic metadata\n");
            return 1;
        }
        bool hmirror = hmirror_value != 0;
        bool vflip = vflip_value != 0;
        bool ok = intrinsic_set(r, K, D,
                                (uint16_t)intr_args.w->ival[0],
                                (uint16_t)intr_args.h->ival[0],
                                (uint16_t)intr_args.pid->ival[0],
                                hmirror, vflip,
                                reproj);
        if (ok) {
            intrinsic_t saved;
            if (!intrinsic_get(r, &saved)) {
                printf("Error: intrinsic write did not read back\n");
                return 1;
            }
            printf("intrinsic saved: crc=%08lx\n",
                   (unsigned long)intrinsic_checksum(&saved));
        }
        return ok ? 0 : 1;
    }
    printf("unknown intrinsic subcommand: %s\n", sub);
    return 1;
}

/* ---- collector command: set the PC UDP destination at runtime ---- */
static struct {
    struct arg_str *ip;
    struct arg_end *end;
} collector_args;

static int cmd_collector(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&collector_args);
    if (nerr != 0 || collector_args.ip->count == 0) {
        printf("collector: see current target in boot log (default %s:%d)\n",
               CONFIG_FTMCS_TELEMETRY_UDP_HOST, CONFIG_FTMCS_TELEMETRY_UDP_PORT);
        return 0;
    }
    centroid_packet_set_host(collector_args.ip->sval[0]);
    camera_udp_set_host(collector_args.ip->sval[0]);
    telemetry_set_host(collector_args.ip->sval[0]);
    return 0;
}

/* ---- rssi command: print current STA RSSI to the joined AP (antenna/link diagnostic) ---- */
static int cmd_rssi(int argc, char **argv)
{
    (void)argc; (void)argv;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("rssi=%d dBm  bssid=%02x:%02x:%02x:%02x:%02x:%02x  ch=%d\n",
               (int)ap.rssi, ap.bssid[0], ap.bssid[1], ap.bssid[2],
               ap.bssid[3], ap.bssid[4], ap.bssid[5], (int)ap.primary);
    } else {
        printf("rssi: STA not associated\n");
    }
    return 0;
}

/* ---- mode command: the single mode switch (local + mesh broadcast) ---- */
static struct {
    struct arg_str *m;       /* live | 1hz | frame | off */
    struct arg_int *value;   /* fps (live) or period_ms (frame) */
    struct arg_str *format;  /* gray | jpeg (UVC only) */
    struct arg_str *timing;  /* sync (UVC only) */
    struct arg_int *sync_fps;/* synchronized UVC capture rate */
    struct arg_lit *nomesh;  /* --nomesh: apply locally only, don't broadcast */
    struct arg_end *end;
} mode_args;

static int str_to_mode(const char *s, strobe_mode_t *out)
{
    if (!s) return 0;
    if (strcmp(s, "live") == 0) { *out = STROBE_LIVE_TRACK; return 1; }
    if (strcmp(s, "1hz") == 0 || strcmp(s, "timesync") == 0) { *out = STROBE_1HZ; return 1; }
    if (strcmp(s, "frame") == 0 || strcmp(s, "udp") == 0) {
        *out = STROBE_FRAME_SYNC; return 1;
    }
    if (strcmp(s, "uvc") == 0) { *out = STROBE_UVC; return 1; }
    if (strcmp(s, "off") == 0 || strcmp(s, "idle") == 0) { *out = STROBE_OFF; return 1; }
    return 0;
}

static int cmd_mode(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&mode_args);
    if (nerr != 0 || mode_args.m->count == 0) {
        printf("mode: %s (threshold=%u width=%luus gpio=%d identify_gpio=%d)\n",
               strobe_gpio_mode_name(strobe_gpio_get_mode()),
               mode_ctrl_get_threshold(),
               (unsigned long)strobe_gpio_get_width_us(),
               CONFIG_FTMCS_STROBE_GPIO, CONFIG_FTMCS_IDENTIFY_RGB_GPIO);
        strobe_gpio_print_clock_status();
        return 0;
    }
    strobe_mode_t m;
    if (!str_to_mode(mode_args.m->sval[0], &m)) {
        printf("usage: mode <live [fps]|1hz|frame [period_ms]|udp [fps] [qvga|vga|fhd]|uvc [320|640|1280|1920] [gray|jpeg] [sync fps]|off> [--nomesh]\n");
        return 1;
    }
    bool udp_requested = strcmp(mode_args.m->sval[0], "udp") == 0;
    uint16_t value = mode_args.value->count ? (uint16_t)mode_args.value->ival[0] : 0;
    if (udp_requested) {
        uint32_t fps = value ? value : 60;
        if (fps < 1 || fps > 120) {
            printf("UDP synchronized FPS must be 1..120\n");
            return 1;
        }
        const char *resolution = mode_args.format->count
            ? mode_args.format->sval[0] : "qvga";
        cam_res_t udp_res = parse_res(resolution);
        if (strcmp(resolution, "qvga") != 0 && strcmp(resolution, "vga") != 0 &&
            strcmp(resolution, "hd") != 0 && strcmp(resolution, "sxga") != 0 &&
            strcmp(resolution, "fhd") != 0) {
            printf("UDP resolution must be qvga, vga, hd, sxga, or fhd\n");
            return 1;
        }
        value = MODE_FRAME_UDP_FLAG |
                (((uint16_t)udp_res << MODE_FRAME_UDP_RES_SHIFT) &
                 MODE_FRAME_UDP_RES_MASK) |
                ((uint16_t)fps & MODE_FRAME_UDP_FPS_MASK);
        if (mode_args.timing->count) {
            if (strcmp(mode_args.timing->sval[0], "free") == 0) value |= MODE_FRAME_UDP_FREE_FLAG;
            else if (strcmp(mode_args.timing->sval[0], "gray") == 0 && udp_res == CAM_RES_QVGA)
                value |= MODE_FRAME_UDP_FREE_FLAG | MODE_FRAME_UDP_GRAY_FLAG;
            else if (strcmp(mode_args.timing->sval[0], "sync") != 0) {
                printf("UDP timing must be free or sync\n");
                return 1;
            }
        }
    }
    if (m == STROBE_UVC && mode_args.format->count) {
        const char *format = mode_args.format->sval[0];
        if (strcmp(format, "jpeg") == 0) {
            value |= MODE_UVC_JPEG_FLAG;
        } else if (strcmp(format, "gray") != 0) {
            printf("UVC format must be gray or jpeg\n");
            return 1;
        }
    }
    if (m == STROBE_LIVE_TRACK && mode_args.format->count) {
        const char *resolution = mode_args.format->sval[0];
        cam_res_t live_res = parse_res(resolution);
        if (strcmp(resolution, "qvga") != 0 && strcmp(resolution, "vga") != 0 &&
            strcmp(resolution, "hd") != 0 && strcmp(resolution, "sxga") != 0 && strcmp(resolution, "fhd") != 0) {
            printf("LIVE_TRACK resolution must be qvga, vga, hd, sxga, or fhd\n");
            return 1;
        }
        mode_ctrl_set_live_resolution(live_res);
    }
    if (m == STROBE_UVC) {
        uint32_t sync_fps = 0;
        if (mode_args.timing->count) {
            if (strcmp(mode_args.timing->sval[0], "sync") != 0) {
                printf("UVC timing must be sync\n");
                return 1;
            }
            sync_fps = mode_args.sync_fps->count
                ? (uint32_t)mode_args.sync_fps->ival[0] : 10;
            if (sync_fps < 1) sync_fps = 1;
            if (sync_fps > 120) sync_fps = 120;
        }
        uint16_t width = value & ~MODE_UVC_JPEG_FLAG;
        if (width == 0) width = (value & MODE_UVC_JPEG_FLAG) ? 640 : 320;
        bool jpeg = (value & MODE_UVC_JPEG_FLAG) != 0;
        if (!mode_ctrl_apply_uvc(width, jpeg, sync_fps)) return 1;
    } else {
        /* Apply non-UVC modes locally first. */
        if (!mode_ctrl_apply(m, value)) return 1;
    }

    /* Broadcast to all other nodes unless --nomesh (legacy flag name). */
    if (!(mode_args.nomesh && mode_args.nomesh->count)) {
        lan_cmd_broadcast(LAN_CMD_MODE, (uint8_t)m, value);
    }
    return 0;
}

/* ---- strobe command: GPIO pulse-width tuning (and manual mode query) ---- */
static struct {
    struct arg_str *sub;     /* width | on | off */
    struct arg_int *width;
    struct arg_end *end;
} strobe_args;

static int cmd_strobe(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&strobe_args);
    if (nerr != 0 || strobe_args.sub->count == 0) {
        printf("strobe mode=%s width=%luus\n",
               strobe_gpio_mode_name(strobe_gpio_get_mode()),
               (unsigned long)strobe_gpio_get_width_us());
        return 0;
    }
    const char *sub = strobe_args.sub->sval[0];
    if (strcmp(sub, "width") == 0) {
        if (strobe_args.width->count == 0) {
            printf("width = %lu us\n", (unsigned long)strobe_gpio_get_width_us());
        } else {
            strobe_gpio_set_width_us((uint32_t)strobe_args.width->ival[0]);
        }
    } else if (strcmp(sub, "thresh") == 0) {
        if (strobe_args.width->count == 0) {
            printf("threshold = %u\n", mode_ctrl_get_threshold());
        } else {
            mode_ctrl_set_threshold((uint8_t)strobe_args.width->ival[0]);
        }
    } else {
        printf("usage: strobe <width [us]|thresh [0-255]>\n");
        return 1;
    }
    return 0;
}

/* ---- sync_cap command: time-synchronized capture scheduler (kept for compat) ---- */
static struct {
    struct arg_str *sub;     /* on | off */
    struct arg_int *period;
    struct arg_str *res;
    struct arg_end *end;
} scap_args;

static int cmd_sync_cap(int argc, char **argv)
{
    int nerr = arg_parse(argc, argv, (void **)&scap_args);
    if (nerr != 0 || scap_args.sub->count == 0) {
        printf("sync_cap %s\n", sync_capture_running() ? "RUNNING" : "stopped");
        return 0;
    }
    const char *sub = scap_args.sub->sval[0];
    if (strcmp(sub, "on") == 0) {
        uint32_t period = (scap_args.period->count)
                        ? (uint32_t)scap_args.period->ival[0]
                        : CONFIG_FTMCS_SYNC_CAPTURE_DEFAULT_PERIOD_MS;
        cam_res_t res = scap_args.res->count
            ? parse_res(scap_args.res->sval[0]) : CAM_RES_VGA;
        bool ok = mode_ctrl_start_sync_preview(period, res);
        const char *res_name = res == CAM_RES_QVGA ? "qvga" :
                               res == CAM_RES_SXGA ? "sxga" :
                               res == CAM_RES_HD ? "hd" :
                               res == CAM_RES_FHD ? "fhd" : "vga";
        printf("sync_cap on %lu %s: %s\n", (unsigned long)period, res_name,
               ok ? "started" : "FAILED");
        return ok ? 0 : 1;
    }
    if (strcmp(sub, "off") == 0) {
        bool ok = mode_ctrl_apply(STROBE_OFF, 0);
        printf("sync_cap off: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    printf("usage: sync_cap <on [period_ms] [qvga|vga|hd|fhd|sxga]|off>\n");
    return 1;
}

void camera_cmd_register_commands(void)
{
    ESP_LOGI(TAG, "registering camera commands...");
    cam_args.sub  = arg_str1(NULL, NULL, "<detect|on|off|exp|orient|tune|stats|rate|scan>", "subcommand");
    cam_args.opts = arg_strn(NULL, NULL, "<option>", 0, 2,
                             "camera options or tuning profile");
    cam_args.a1i  = arg_int0(NULL, NULL, "<level>", "exposure level");
    cam_args.end  = arg_end(4);
    const esp_console_cmd_t cam_cmd = {
        .command = "cam",
        .help = "Camera control: detect, stream, exposure, and persistent sensor tuning.",
        .func = &cmd_cam,
        .argtable = &cam_args,
    };
    esp_err_t e1 = esp_console_cmd_register(&cam_cmd);
    ESP_LOGI(TAG, "cam register: %s", e1 == ESP_OK ? "OK" : esp_err_to_name(e1));

    intr_args.sub    = arg_str1(NULL, NULL, "<get|clear|set>", "subcommand");
    intr_args.res    = arg_str0(NULL, NULL, "<qvga|vga|hd|fhd|sxga>", "resolution");
    intr_args.K      = arg_str0(NULL, NULL, "<Kcsv>", "9 csv doubles (fx,0,cx,...)");
    intr_args.D      = arg_str0(NULL, NULL, "<Dcsv>", "8 csv distortion coeffs");
    intr_args.w      = arg_int0(NULL, NULL, "<w>", "calib image width");
    intr_args.h      = arg_int0(NULL, NULL, "<h>", "calib image height");
    intr_args.pid    = arg_int0(NULL, NULL, "<pid>", "sensor pid (hex ok)");
    intr_args.reproj = arg_dbl0(NULL, NULL, "<reproj>", "RMS reprojection error px");
    intr_args.hmirror= arg_int0(NULL, NULL, "<hmirror>", "sensor horizontal mirror (0/1)");
    intr_args.vflip  = arg_int0(NULL, NULL, "<vflip>", "sensor vertical flip (0/1)");
    intr_args.end    = arg_end(10);
    const esp_console_cmd_t intr_cmd = {
        .command = "intrinsic",
        .help = "Show / clear / set camera intrinsics stored in NVS.",
        .func = &cmd_intrinsic,
        .argtable = &intr_args,
    };
    esp_err_t e2 = esp_console_cmd_register(&intr_cmd);
    ESP_LOGI(TAG, "intrinsic register: %s", e2 == ESP_OK ? "OK" : esp_err_to_name(e2));

    mode_args.m      = arg_str0(NULL, NULL, "<live|1hz|frame|udp|uvc|off>", "operating mode");
    const esp_console_cmd_t timing_cmd = {
        .command = "timing", .help = "Timing experiment: config <task|high|esp|gpt|gptpair|mcpwm> <ftm|frozen|tsf|mac>; dump (while OFF)",
        .func = &strobe_gpio_timing_command,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&timing_cmd));
    mode_args.value  = arg_int0(NULL, NULL, "<fps|period_ms|width>", "fps (live), period (frame), or UVC width (320/640/1280/1920)");
    mode_args.format = arg_str0(NULL, NULL, "<gray|jpeg>", "native Y8 or MJPEG USB wire format");
    mode_args.timing = arg_str0(NULL, NULL, "<sync>", "globally align UVC exposure ticks");
    mode_args.sync_fps = arg_int0(NULL, NULL, "<fps>", "synchronized UVC capture rate");
    mode_args.nomesh = arg_lit0(NULL, "nomesh", "apply locally only, don't broadcast");
    mode_args.end    = arg_end(7);
    const esp_console_cmd_t mode_cmd = {
        .command = "mode",
        .help = "Switch operating mode (propagates to all nodes via LAN UDP "
                "unless --nomesh). live [fps] | 1hz | frame [period_ms] | udp [fps] [qvga|vga|fhd] | uvc [width] [gray|jpeg] [sync fps] | off.",
        .func = &cmd_mode,
        .argtable = &mode_args,
    };
    esp_err_t e3 = esp_console_cmd_register(&mode_cmd);
    ESP_LOGI(TAG, "mode register: %s", e3 == ESP_OK ? "OK" : esp_err_to_name(e3));

    strobe_args.sub   = arg_str0(NULL, NULL, "<width|thresh>", "tuning subcommand");
    strobe_args.width = arg_int0(NULL, NULL, "<value>", "us (width) or 0-255 (thresh)");
    strobe_args.end   = arg_end(3);
    const esp_console_cmd_t strobe_cmd = {
        .command = "strobe",
        .help = "Strobe GPIO tuning: width <us> | thresh <0-255>.",
        .func = &cmd_strobe,
        .argtable = &strobe_args,
    };
    esp_err_t e3b = esp_console_cmd_register(&strobe_cmd);
    ESP_LOGI(TAG, "strobe register: %s", e3b == ESP_OK ? "OK" : esp_err_to_name(e3b));

    scap_args.sub    = arg_str1(NULL, NULL, "<on|off>", "subcommand");
    scap_args.period = arg_int0(NULL, NULL, "<period_ms>", "sync period (default 100)");
    scap_args.res    = arg_str0(NULL, NULL, "<vga|sxga>", "resolution");
    scap_args.end    = arg_end(4);
    const esp_console_cmd_t scap_cmd = {
        .command = "sync_cap",
        .help = "Time-synchronized capture: all nodes grab at the same global tick.",
        .func = &cmd_sync_cap,
        .argtable = &scap_args,
    };
    esp_err_t e4 = esp_console_cmd_register(&scap_cmd);
    ESP_LOGI(TAG, "sync_cap register: %s", e4 == ESP_OK ? "OK" : esp_err_to_name(e4));

    collector_args.ip = arg_str0(NULL, NULL, "<ip>", "PC collector IP for IRP1 packets");
    collector_args.end = arg_end(2);
    const esp_console_cmd_t coll_cmd = {
        .command = "collector",
        .help = "Set the PC UDP destination for centroid packets (runtime).",
        .func = &cmd_collector,
        .argtable = &collector_args,
    };
    esp_err_t e5 = esp_console_cmd_register(&coll_cmd);
    ESP_LOGI(TAG, "collector register: %s", e5 == ESP_OK ? "OK" : esp_err_to_name(e5));

    const esp_console_cmd_t rssi_cmd = {
        .command = "rssi",
        .help = "Print current STA RSSI (dBm), BSSID, channel to the joined AP.",
        .func = &cmd_rssi,
    };
    esp_err_t e6 = esp_console_cmd_register(&rssi_cmd);
    ESP_LOGI(TAG, "rssi register: %s", e6 == ESP_OK ? "OK" : esp_err_to_name(e6));
}
