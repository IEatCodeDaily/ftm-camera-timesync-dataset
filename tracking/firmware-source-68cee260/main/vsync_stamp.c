/*
 * vsync_stamp.c -- see vsync_stamp.h.
 *
 * A frame's capture timestamp was esp_timer_get_time() read by cam_task when it
 * started the frame: VSYNC interrupt + event queue + task latency after the
 * edge, a different amount every frame. MCPWM capture latches a free-running
 * 80 MHz counter on the VSYNC edge in hardware, so the edge time no longer
 * depends on when any code ran.
 *
 * Channel 0 latches rising and channel 1 falling edges. The edge the driver's
 * VSYNC interrupt answered is the latest one at or before that interrupt's own
 * stamp, so no sensor polarity setting has to be known or kept in step.
 * Channel 2 is a software capture that pairs the counter with esp_timer. MCPWM
 * capture and esp_timer both derive from the 40 MHz crystal, so the pairing
 * holds without drift over the microseconds it spans.
 *
 * Group 1 belongs to strobe_gpio.c; this owns group 0's capture timer.
 */
#include "vsync_stamp.h"
#include "camera_capture.h"

#include <limits.h>
#include "driver/mcpwm_cap.h"
#include "hal/mcpwm_ll.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "vsync_stamp";

#define GROUP 0
/* ponytail: the driver's VSYNC interrupt runs microseconds after its edge; an
 * edge 1 ms or more before it belongs to another VSYNC. */
#define MAX_ISR_LAG_US 1000
#define PAIR_SLACK_NS 2000      /* esp_timer is 1 us; allow pairing rounding */

extern void cam_hal_set_vsync_stamp(int64_t (*fn)(int64_t vsync_isr_us));

static mcpwm_cap_timer_handle_t s_timer;
static mcpwm_cap_channel_handle_t s_ch[3];
static int s_pin = -1;
static uint32_t s_ticks_per_us;
static bool s_on;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vsync_stamp_stats_t s_st;

static void stats_reset_locked(void)
{
    bool on = s_st.on;
    s_st = (vsync_stamp_stats_t){.on = on,
        .sw_lag_min_us = INT32_MAX, .isr_lag_min_us = INT32_MAX,
        .sw_lag_max_us = INT32_MIN, .isr_lag_max_us = INT32_MIN};
}

/* cam_task context (cam_hal cam_start_frame). Returns 0 to keep the
 * software stamp. */
static int64_t stamp(int64_t vsync_isr_us)
{
    mcpwm_dev_t *dev = MCPWM_LL_GET_HW(GROUP);
    const uint32_t edge[2] = {mcpwm_ll_capture_get_value(dev, 0),
                              mcpwm_ll_capture_get_value(dev, 1)};
    const int64_t before = esp_timer_get_time();
    mcpwm_ll_trigger_soft_capture(dev, 2);
    const int64_t after = esp_timer_get_time();
    const uint32_t ref = mcpwm_ll_capture_get_value(dev, 2);
    const int64_t mid_ns = (before + after) * 500;
    const int64_t isr_ns = vsync_isr_us * 1000;

    int64_t best_ns = 0;
    int best = -1;
    for (int i = 0; i < 2; i++) {
        /* unsigned difference: correct across the 53.7 s counter wrap */
        const int64_t age_ns = (int64_t)(uint32_t)(ref - edge[i]) * 1000 / s_ticks_per_us;
        const int64_t t_ns = mid_ns - age_ns;
        if (t_ns > isr_ns + PAIR_SLACK_NS || isr_ns - t_ns > MAX_ISR_LAG_US * 1000LL) continue;
        if (best < 0 || t_ns > best_ns) { best_ns = t_ns; best = i; }
    }

    portENTER_CRITICAL(&s_lock);
    if (best < 0) {
        s_st.misses++;
        portEXIT_CRITICAL(&s_lock);
        return 0;
    }
    const int64_t t_us = (best_ns + 500) / 1000;
    const int32_t sw = (int32_t)(after - t_us), isr = (int32_t)(vsync_isr_us - t_us);
    s_st.hits++;
    if (best == 0) s_st.rise++; else s_st.fall++;
    s_st.sw_lag_sum_us += sw;
    if (sw < s_st.sw_lag_min_us) s_st.sw_lag_min_us = sw;
    if (sw > s_st.sw_lag_max_us) s_st.sw_lag_max_us = sw;
    if (isr < s_st.isr_lag_min_us) s_st.isr_lag_min_us = isr;
    if (isr > s_st.isr_lag_max_us) s_st.isr_lag_max_us = isr;
    portEXIT_CRITICAL(&s_lock);
    return t_us;
}

static bool init(int pin)
{
    mcpwm_capture_timer_config_t tc = {.group_id = GROUP, .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT};
    if (mcpwm_new_capture_timer(&tc, &s_timer) != ESP_OK) return false;
    /* Created in order on a fresh timer, so these are channels 0, 1, 2 -- the
     * indices the LL reads in stamp() rely on. */
    const mcpwm_capture_channel_config_t cfg[3] = {
        {.gpio_num = pin, .prescale = 1, .flags.pos_edge = 1},
        {.gpio_num = pin, .prescale = 1, .flags.neg_edge = 1},
        {.gpio_num = -1,  .prescale = 1},   /* software capture only */
    };
    for (int i = 0; i < 3; i++) {
        if (mcpwm_new_capture_channel(s_timer, &cfg[i], &s_ch[i]) != ESP_OK ||
            mcpwm_capture_channel_enable(s_ch[i]) != ESP_OK) return false;
    }
    uint32_t hz = 0;
    if (mcpwm_capture_timer_enable(s_timer) != ESP_OK ||
        mcpwm_capture_timer_start(s_timer) != ESP_OK ||
        mcpwm_capture_timer_get_resolution(s_timer, &hz) != ESP_OK || hz < 1000000) return false;
    s_ticks_per_us = hz / 1000000;
    s_pin = pin;
    ESP_LOGI(TAG, "VSYNC GPIO %d -> MCPWM%d capture at %lu Hz", pin, GROUP, (unsigned long)hz);
    return true;
}

bool vsync_stamp_set(bool on)
{
    if (on) {
        const int pin = camera_vsync_pin();
        if (pin < 0) { ESP_LOGE(TAG, "camera pinout not detected yet"); return false; }
        /* ponytail: set up once per boot; the detected pinout is board wiring
         * and does not change, so there is no teardown path. */
        if (!s_timer && !init(pin)) { ESP_LOGE(TAG, "MCPWM capture init failed"); return false; }
        if (pin != s_pin) { ESP_LOGE(TAG, "VSYNC pin changed %d -> %d; reboot", s_pin, pin); return false; }
    }
    portENTER_CRITICAL(&s_lock);
    s_on = on;
    s_st.on = on;
    stats_reset_locked();
    portEXIT_CRITICAL(&s_lock);
    cam_hal_set_vsync_stamp(on ? stamp : NULL);
    return true;
}

void vsync_stamp_stats(vsync_stamp_stats_t *out, bool reset)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    out->on = s_on;
    if (reset) stats_reset_locked();
    portEXIT_CRITICAL(&s_lock);
}
