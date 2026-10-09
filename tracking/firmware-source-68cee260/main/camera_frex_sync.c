/*
 * camera_frex_sync.c -- see camera_frex_sync.h.
 *
 * Uses the esp32-camera sensor_t::set_reg() API to write OV5640 FREX registers
 * over SCCB (I2C). The sensor handle is obtained from esp_camera_sensor_get().
 */
#include "camera_frex_sync.h"
#include "camera_capture.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include <limits.h>
#include "sensor.h"
#include "strobe_gpio.h"
#include "driver/gptimer.h"
#include "driver/i2c_master.h"
#include "hal/i2c_ll.h"
#include "sdkconfig.h"

static const char *TAG = "frex_sync";
static bool s_enabled = false;

/* OV5640 FREX register addresses (from datasheet + Linux kernel). */
#define REG_PAD_OE01        0x3017
#define REG_PAD_OE02        0x3018
#define REG_PAD_CTRL        0x3016
#define REG_ANALOG_CTRL     0x3709
#define REG_FREX_EXP_HH     0x3B01   /* exposure [23:16] */
#define REG_FREX_SHUT_H     0x3B02   /* shutter delay [12:8] */
#define REG_FREX_SHUT_L     0x3B03   /* shutter delay [7:0] */
#define REG_FREX_EXP_H      0x3B04   /* exposure [15:8] */
#define REG_FREX_EXP_L      0x3B05   /* exposure [7:0] */
#define REG_FREX_CTRL       0x3B06   /* frame delay [7:4], strobe width [3:0] */
#define REG_FREX_MODE       0x3B07   /* 0x00=mode0, 0x01=mode1(Mode2 I2C trig) */
#define REG_FREX_REQUEST    0x3B08   /* bit[0]=software FREX trigger */

/* Helper: write a full 8-bit register value via the sensor's set_reg API. */
static bool write_sensor_reg(uint16_t reg, uint8_t value)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_reg) {
        ESP_LOGE(TAG, "no sensor handle");
        return false;
    }
    int ret = s->set_reg(s, reg, 0xFF, value);
    if (ret < 0) {
        ESP_LOGE(TAG, "set_reg(0x%04X, 0x%02X) failed: %d", reg, value, ret);
        return false;
    }
    return true;
}

/* FREX owns the shutter, so the operator's tuned exposure must land in the
 * FREX exposure registers or the exposure control does nothing while
 * synchronized. Must stay BELOW the frame's row count (VTS) or the shutter is
 * still open when the next readout starts: VTS is 500 at QVGA and 744+ at
 * VGA/720p, so cap at 80% of the smaller.
 * ponytail: single clamp, not per-resolution - split it only if a mode appears
 * whose VTS drops below 500. */
#define FREX_MAX_EXPOSURE_LINES 400U

uint16_t frex_sync_tuned_exposure_lines(void)
{
    camera_tuning_t tuning;
    camera_get_tuning(&tuning);
    /* Automatic (-1) has no meaning once FREX owns the shutter: there is no
     * AEC loop driving it. Use the cap, a correct mid-scale exposure for this
     * rig rather than the sensor's 1024-line reset default. */
    if (tuning.exposure_lines < 1) return FREX_MAX_EXPOSURE_LINES;
    if ((uint32_t)tuning.exposure_lines > FREX_MAX_EXPOSURE_LINES) {
        return FREX_MAX_EXPOSURE_LINES;
    }
    return (uint16_t)tuning.exposure_lines;
}

static bool frex_write_exposure(uint32_t exp)
{
    return write_sensor_reg(REG_FREX_EXP_HH, (exp >> 16) & 0xFF) &&
           write_sensor_reg(REG_FREX_EXP_H,  (exp >> 8)  & 0xFF) &&
           write_sensor_reg(REG_FREX_EXP_L,   exp        & 0xFF);
}

/* Caller already holds the sensor mutation lock (camera_apply_tuning). */
bool frex_sync_reapply_exposure_ordinary_owned(void)
{
    if (!s_enabled) return true;
    uint16_t lines = frex_sync_tuned_exposure_lines();
    if (!frex_write_exposure(lines)) return false;
    ESP_LOGI(TAG, "FREX exposure = %u Tlines (re-tuned)", lines);
    return true;
}

static bool frex_sync_enable_ordinary_owned(uint16_t exposure_lines)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "camera not initialised");
        return false;
    }

    /* FREX Mode 2 - starting an exposure by writing 0x3B08 over I2C - is an
     * OV5640 feature. The 0x3B00-0x3B08 block does not exist on other parts:
     * OV2640, for instance, has a bank-switched register map entirely, and its
     * frame-exposure mode is driven by the external FREX and EXPST_B PINS,
     * which no pin preset in camera_capture.c wires up.
     *
     * Writing this block to a different sensor would poke arbitrary registers
     * and report success, so synchronized mode would silently produce
     * free-running frames while claiming microsecond alignment. Refuse
     * instead, loudly. */
    if (s->id.PID != OV5640_PID) {
        ESP_LOGE(TAG,
                 "FREX sync requires OV5640 (PID 0x%04x); this sensor is "
                 "0x%04x. Synchronized capture is unavailable - use "
                 "free-running and align by capture_us.",
                 OV5640_PID, s->id.PID);
        return false;
    }

    /* OmniVision's Mode-2/I2C sequence makes FREX an output (it carries the
     * shutter-control signal). The previous 0x7f value forced it to an input,
     * which is Mode 1 wiring and caused alternating/lost software triggers. */
    if (!write_sensor_reg(REG_PAD_CTRL, 0x02)) return false;
    if (!write_sensor_reg(REG_PAD_OE01, 0xFF)) return false;  /* FREX=output */
    if (!write_sensor_reg(REG_PAD_OE02, 0xFC)) return false;  /* D[5:0] out */
    if (!write_sensor_reg(REG_ANALOG_CTRL, 0x10)) return false;

    /* 2. Shutter delay: time from the FREX request to the shutter closing,
     * in units of 64 sclk cycles. The datasheet reset value is 0x0008, and
     * this forced 0x0000 instead - outside the part's documented operating
     * point for no stated reason. Bench sweep found the image flat across
     * 0..1024 (4779..4996 bright px), so zero buys nothing, and the default
     * is the value OmniVision validated. Keep the sensor's own number. */
    if (!write_sensor_reg(REG_FREX_SHUT_H, 0x00)) return false;
    if (!write_sensor_reg(REG_FREX_SHUT_L, 0x08)) return false;

    /* 3. Exposure time in Tline (row periods). A fixed short exposure is
     * required for 60 Hz: the sensor default is 0x0400 (1024 lines), already
     * longer than a 16.667 ms cadence with our QVGA timing. */
    if (exposure_lines == 0) exposure_lines = frex_sync_tuned_exposure_lines();
    if (!frex_write_exposure(exposure_lines)) return false;
    ESP_LOGI(TAG, "FREX exposure = %u Tlines", exposure_lines);

    /* No additional frame delay: begin exposure at this request. Keep the
     * documented four-line strobe width. */
    if (!write_sensor_reg(REG_FREX_CTRL, 0x04)) return false;

    /* 4. FREX mode select. Bits[1:0] are the ONLY functional field:
     *   00 = FREX strobe mode0, 01 = FREX strobe mode1, 1x = rolling strobe.
     *
     * This wrote 0x09 with a comment claiming bit 3 "enables the FREX
     * function". That is wrong, and it silently disabled the whole feature:
     * 0x3B07's RESET VALUE is 0x08, so bit 3 is simply part of the default,
     * not an enable. Leaving it set keeps the frame-exposure path bypassed.
     *
     * Measured on the bench against a spinning fan (VGA, fixed gain/scene),
     * counting pixels at >=85% of peak - a frozen marker paints few, a
     * smeared one paints many:
     *   rolling (FREX off) 79756 px   blades smeared into a featureless disc
     *   0x09 (this bug)    83340 px   IDENTICAL to rolling - FREX inert
     *   0x01 (fixed)        4887 px   16x less smear, blades frozen
     * Exposure only responds under 0x01 (4887 px at 1 Tline -> 2330 at 768),
     * which is the pseudo-global-shutter behaviour this sensor is here for. */
    if (!write_sensor_reg(REG_FREX_MODE, 0x01)) return false;

    /* 5. Clear any pending FREX request. */
    if (!write_sensor_reg(REG_FREX_REQUEST, 0x00)) return false;

    s_enabled = true;
    ESP_LOGI(TAG, "FREX Mode 2 enabled (software I2C trigger, no external pin)");
    return true;
}

bool frex_sync_enable(uint16_t exposure_lines)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    bool result = frex_sync_enable_ordinary_owned(exposure_lines);
    camera_sensor_mutation_end();
    return result;
}

/* sccb-ng.c (esp32-camera private API): one raw 16-bit-address write. */
extern int SCCB_Write16(uint8_t slv_addr, uint16_t reg, uint8_t data);

static bool frex_sync_trigger_ordinary_owned(void)
{
    if (!s_enabled) return false;
    /* Installed modules keep 0x3B08[0] latched after the triggered frame has
     * completed. A repeated write of 1 has no edge and produces no exposure.
     * Clear the completed prior request immediately before asserting the next
     * one; never clear it immediately after assertion, which can cancel the
     * in-flight exposure.
     * Exposure starts when the set write lands, so these are raw writes:
     * sensor_t::set_reg() reads each register back first, which doubled the
     * SCCB traffic between the slot and the exposure (measured 2.3-4.75 ms
     * slot->landed, jittering with the I2C driver). */
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    if (SCCB_Write16(s->slv_addr, REG_FREX_REQUEST, 0x00) != 0) return false;
    esp_rom_delay_us(20);
    if (SCCB_Write16(s->slv_addr, REG_FREX_REQUEST, 0x01) != 0) return false;
    /* Logic-analyzer reference: pulse at the commanded exposure instant. */
    strobe_gpio_fire();
    return true;
}

/* cam_hal.c (patch esp32-camera-real-vsync-start): the driver starts a frame
 * only at the image VSYNC of the latest trigger. 0 = untriggered free-run. */
extern void cam_hal_set_trigger_us(int64_t us);

bool frex_sync_trigger(void)
{
    if (!camera_sensor_mutation_begin()) { return false; }
    cam_hal_set_trigger_us(esp_timer_get_time());
    bool result = frex_sync_trigger_ordinary_owned();
    camera_sensor_mutation_end();
    return result;
}

bool frex_sync_active(void) { return s_enabled; }

/* ---- hardware-timed trigger (`cam hwtrig on`) -------------------------------
 * The software trigger above reaches 0x3B08 after a spin-wait, the sensor
 * mutex and the I2C driver (semaphores, interrupt, task wake-up), so the
 * instant the exposure starts moves by tens of microseconds per frame. Here
 * the request write is preloaded into the I2C controller's command list ahead
 * of the slot and a GPTimer alarm interrupt starts it: the bus transfer then
 * has a fixed duration, leaving only interrupt latency (~1-2 us) as jitter.
 *
 * The clear (0x3B08 = 0) moves from 20 us before the request to `lead` before
 * it, still after the previous frame completed, as the register needs.
 *
 * This bypasses the I2C driver for one transaction, so it relies on two
 * things: every sensor access in this firmware holds camera_sensor_mutation
 * (held here from the clear to completion), and the driver masks its I2C
 * interrupt between its synchronous transactions (raw status is cleared here
 * afterwards so the driver's next transaction cannot see stale events). */
#if CONFIG_SCCB_HARDWARE_I2C_PORT1
#define SCCB_PORT 1
#else
#define SCCB_PORT 0
#endif
/* ponytail: clear write (~150 us through the driver) + preload + margin.
 * `cam hwtrig stats` counts late slots if this is too small. */
#define HWTRIG_LEAD_US 1500
#define HWTRIG_MIN_ARM_US 20
#define HWTRIG_DONE_US 3000     /* 4 bytes at 400 kHz is ~100 us */

static bool s_hw;
static gptimer_handle_t s_hw_timer;
static volatile int64_t s_hw_fired_us;
static frex_hw_stats_t s_hw_st;

static bool IRAM_ATTR hw_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *ctx)
{
    i2c_dev_t *dev = I2C_LL_GET_HW(SCCB_PORT);
    i2c_ll_update(dev);
    i2c_ll_start_trans(dev);
    s_hw_fired_us = esp_timer_get_time();
    return false;
}

/* Created lazily from the trigger task, so the alarm interrupt lands on the
 * camera core rather than the console's. */
static bool hw_timer_init(void)
{
    gptimer_config_t config = {.clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP,
                               .resolution_hz = 1000000, .intr_priority = 3};
    gptimer_event_callbacks_t cb = {.on_alarm = hw_alarm};
    if (gptimer_new_timer(&config, &s_hw_timer) != ESP_OK) { s_hw_timer = NULL; return false; }
    if (gptimer_register_event_callbacks(s_hw_timer, &cb, NULL) != ESP_OK ||
        gptimer_enable(s_hw_timer) != ESP_OK || gptimer_start(s_hw_timer) != ESP_OK) {
        ESP_LOGE(TAG, "hwtrig GPTimer start failed");
        return false;   /* ponytail: leaks the timer on a failure that never recovers anyway */
    }
    return true;
}

static void hw_minmax(int32_t v, int32_t *lo, int32_t *hi)
{
    if (v < *lo) *lo = v;
    if (v > *hi) *hi = v;
}

static void hw_stats_reset(void)
{
    s_hw_st = (frex_hw_stats_t){.fire_min_us = INT32_MAX, .fire_max_us = INT32_MIN,
                                .done_min_us = INT32_MAX, .done_max_us = INT32_MIN};
}

/* Wait for the started transfer; returns its completion time or 0. */
static int64_t hw_wait_done(i2c_dev_t *dev, int64_t until_us, bool *nack)
{
    while (esp_timer_get_time() < until_us) {
        if (dev->int_raw.nack_int_raw) { *nack = true; return 0; }
        if (dev->int_raw.time_out_int_raw || dev->int_raw.arbitration_lost_int_raw) return 0;
        if (dev->int_raw.trans_complete_int_raw) return esp_timer_get_time();
    }
    return 0;
}

static bool hw_trigger_owned(int64_t deadline_us)
{
    if (!s_enabled) return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    if (!s_hw_timer && !hw_timer_init()) return false;
    if (SCCB_Write16(s->slv_addr, REG_FREX_REQUEST, 0x00) != 0) return false;

    i2c_dev_t *dev = I2C_LL_GET_HW(SCCB_PORT);
    if (i2c_ll_is_bus_busy(dev)) { s_hw_st.busy++; return false; }
    const uint8_t frame[4] = {(uint8_t)(s->slv_addr << 1), REG_FREX_REQUEST >> 8,
                              REG_FREX_REQUEST & 0xFF, 0x01};
    i2c_ll_txfifo_rst(dev);
    i2c_ll_rxfifo_rst(dev);
    i2c_ll_clear_intr_mask(dev, I2C_LL_INTR_MASK);
    i2c_ll_write_txfifo(dev, frame, sizeof(frame));
    i2c_ll_master_write_cmd_reg(dev, (i2c_ll_hw_cmd_t){.op_code = I2C_LL_CMD_RESTART}, 0);
    i2c_ll_master_write_cmd_reg(dev, (i2c_ll_hw_cmd_t){.op_code = I2C_LL_CMD_WRITE,
                                                       .byte_num = sizeof(frame), .ack_en = 1}, 1);
    i2c_ll_master_write_cmd_reg(dev, (i2c_ll_hw_cmd_t){.op_code = I2C_LL_CMD_STOP}, 2);

    /* Pair the GPTimer count with esp_timer: keep the narrowest of 4 reads. */
    uint64_t count = 0;
    int64_t paired = 0, narrowest = INT64_MAX;
    for (int i = 0; i < 4; i++) {
        uint64_t c;
        const int64_t before = esp_timer_get_time();
        gptimer_get_raw_count(s_hw_timer, &c);
        const int64_t after = esp_timer_get_time();
        if (after - before < narrowest) { narrowest = after - before; paired = before + narrowest / 2; count = c; }
    }
    const int64_t lead = deadline_us - paired;
    if (lead < HWTRIG_MIN_ARM_US || narrowest > 3) {
        i2c_ll_txfifo_rst(dev);
        s_hw_st.late++;
        return false;
    }
    cam_hal_set_trigger_us(deadline_us);
    s_hw_fired_us = 0;
    gptimer_alarm_config_t alarm = {.alarm_count = count + (uint64_t)lead};
    if (gptimer_set_alarm_action(s_hw_timer, &alarm) != ESP_OK) return false;

    bool nack = false;
    int64_t done = hw_wait_done(dev, deadline_us + HWTRIG_DONE_US, &nack);
    if (!s_hw_fired_us) {
        /* Never fired: disarm, then re-check the race with a last-moment fire. */
        gptimer_set_alarm_action(s_hw_timer, NULL);
        if (s_hw_fired_us) done = hw_wait_done(dev, esp_timer_get_time() + HWTRIG_DONE_US, &nack);
    }
    const bool fired = s_hw_fired_us != 0;
    if (nack) {
        /* Same recovery as the driver: finish the transaction with a STOP. */
        i2c_ll_clear_intr_mask(dev, I2C_LL_INTR_MASK);
        i2c_ll_master_write_cmd_reg(dev, (i2c_ll_hw_cmd_t){.op_code = I2C_LL_CMD_STOP}, 0);
        i2c_ll_update(dev);
        i2c_ll_start_trans(dev);
        bool ignored = false;
        hw_wait_done(dev, esp_timer_get_time() + HWTRIG_DONE_US, &ignored);
        s_hw_st.nack++;
    } else if (fired && !done) {
        i2c_master_bus_handle_t bus;
        if (i2c_master_get_bus_handle(SCCB_PORT, &bus) == ESP_OK) i2c_master_bus_reset(bus);
    }
    i2c_ll_clear_intr_mask(dev, I2C_LL_INTR_MASK);
    if (!fired || !done || nack) {
        if (!nack) s_hw_st.timeout++;
        return false;
    }
    s_hw_st.ok++;
    hw_minmax((int32_t)(s_hw_fired_us - deadline_us), &s_hw_st.fire_min_us, &s_hw_st.fire_max_us);
    hw_minmax((int32_t)(done - deadline_us), &s_hw_st.done_min_us, &s_hw_st.done_max_us);
    /* Logic-analyzer reference, after the request landed. */
    strobe_gpio_fire();
    return true;
}

int64_t frex_sync_lead_us(void) { return s_hw ? HWTRIG_LEAD_US : 0; }

bool frex_sync_trigger_at(int64_t deadline_us)
{
    if (!s_hw) return frex_sync_trigger();
    if (!camera_sensor_mutation_begin()) return false;
    const bool result = hw_trigger_owned(deadline_us);
    camera_sensor_mutation_end();
    return result;
}

void frex_sync_hw_set(bool on)
{
    if (!camera_sensor_mutation_begin()) return;
    s_hw = on;
    hw_stats_reset();
    camera_sensor_mutation_end();
}

bool frex_sync_hw_get(void) { return s_hw; }

void frex_sync_hw_stats(frex_hw_stats_t *out, bool reset)
{
    if (!camera_sensor_mutation_begin()) { *out = (frex_hw_stats_t){0}; return; }
    *out = s_hw_st;
    if (reset) hw_stats_reset();
    camera_sensor_mutation_end();
}

/* Read back the FREX request bit.
 *
 * Returns 1 while an asserted exposure is still outstanding, 0 once the
 * sensor has retired it (the triggered frame has been read out), and -1 if
 * the register could not be read or FREX is not enabled.
 *
 * This is the only way to tell a FREX exposure from an ordinary free-running
 * readout with certainty. Arrival time cannot do it: the sensor runs faster
 * than the trigger, so a free-running frame can land inside any timing window
 * chosen, and the nodes were observed publishing frames whose timestamps
 * PRECEDED their own trigger. */
int frex_sync_request_pending(void)
{
    if (!s_enabled) return -1;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->get_reg) return -1;
    int value = s->get_reg(s, REG_FREX_REQUEST, 0xFF);
    if (value < 0) return -1;
    return (value & 0x01) ? 1 : 0;
}

static void frex_sync_disable_ordinary_owned(void)
{
    if (!s_enabled) return;
    /* Restore: FREX mode 0 (autonomous), clear request. */
    write_sensor_reg(REG_FREX_MODE, 0x00);
    write_sensor_reg(REG_FREX_REQUEST, 0x00);
    s_enabled = false;
    cam_hal_set_trigger_us(0);
    ESP_LOGI(TAG, "FREX disabled (back to autonomous streaming)");
}

void frex_sync_disable(void)
{
    if (!camera_sensor_mutation_begin()) { return; }
    frex_sync_disable_ordinary_owned();
    camera_sensor_mutation_end();
    
}
