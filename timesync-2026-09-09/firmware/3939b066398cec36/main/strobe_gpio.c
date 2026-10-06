/*
 * strobe_gpio.c -- see strobe_gpio.h.
 *
 * Owns one GPIO exclusively. The three modes share the pin:
 *   Camera capture modes    : the grab path calls strobe_gpio_fire() -> a short
 *                             high pulse of s_width_us via busy-wait.
 *   1HZ                     : an internal task toggles the pin once per
 *                             predicted global second (master = own clock,
 *                             initiator = affine-mapped), 50 ms half-period.
 *   OFF                     : pin held low.
 *
 * Switching mode tears down the previous mode's task (if any) so there is never
 * contention on the pin.
 */
#include "strobe_gpio.h"
#include "strobe_clock.h"
#include "simple_ntp.h"

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/mcpwm_prelude.h"
#include "hal/mcpwm_ll.h"
#include "esp_wifi.h"
#include <string.h>
#include "task_cores.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_private/wifi.h"
#include <inttypes.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "clock_model.h"
#include "wifi_setup.h"

static const char *TAG = "strobe_gpio";

#define PULSE_HALF_PERIOD_US 50000   /* 50 ms high, 50 ms low for the 1 Hz mode */

static int           s_gpio = -1;
static strobe_mode_t s_mode = STROBE_OFF;
static uint32_t      s_width_us = 10;
static TaskHandle_t  s_hz_task = NULL;
static volatile bool s_hz_running = false;
static volatile bool s_load_monitor = false;
static void start_hz(void);
static void stop_hz(void);

/* FTM t1..t4 use the Wi-Fi MAC clock, not esp_timer's boot epoch. Bracket a
 * MAC read with esp_timer reads and keep the narrowest pair. Modem/light sleep
 * must remain disabled (wifi_setup uses WIFI_PS_NONE). One model snapshot is
 * used for both directions of the affine conversion. */
static bool sample_mac_clock(const clock_model_t *m, bool reference,
                             int64_t *mac_us, int64_t *timer_us, int64_t *span_us)
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
    uint64_t anchor = reference ? (uint64_t)best_timer : m->ref_local_ps / 1000000ULL;
    int64_t extended = strobe_mac_extend(best_mac, anchor);
    if (extended < 0 || best_span > 10) return false;
    /* An initiator fit should have a recent MAC epoch. Never apply a stale or
     * mismatched clock domain just to produce plausible-looking pulses. */
    if (!reference && (extended < (int64_t)anchor || extended - (int64_t)anchor > 30000000)) return false;
    *mac_us = extended; *timer_us = best_timer; *span_us = best_span;
    return true;
}

void strobe_gpio_print_clock_status(void)
{
    clock_model_t m = clock_model_get();
    bool reference = wifi_is_master();
    int64_t mac, timer, span;
    if ((!reference && !m.valid) || !sample_mac_clock(&m, reference, &mac, &timer, &span)) {
        printf("clock_source=wifi_mac clock_ready=0 reference=%d\n", reference);
        return;
    }
    printf("clock_source=wifi_mac clock_ready=1 reference=%d mac_us=%" PRId64
           " esp_us=%" PRId64 " mac_minus_esp_us=%" PRId64 " read_span_us=%" PRId64 "\n",
           reference, mac, timer, mac - timer, span);
}

/* ---- us <-> ps helpers for the 1 Hz model-mapped edge ---- */
static inline uint64_t us_to_ps(uint64_t us) { return us * 1000000ULL; }
static inline uint64_t ps_to_us(uint64_t ps) { return ps / 1000000ULL; }

static void configure_pin(int gpio_num)
{
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
}


/* Experiment configuration is deliberately volatile: OTA/reboot restores FTM. */
static int s_backend=5; /* MCPWM hardware edges; all comparison backends remain selectable. */
static int s_source;  /* 0 FTM, 1 frozen fit, 2 AP TSF, 3 local MAC, 4 software NTP */
static const char *backend_names[] = {"task", "high", "esp", "gpt", "gptpair", "mcpwm"};
static const char *source_names[] = {"ftm", "frozen", "tsf", "mac", "ntp"};
typedef struct {
    int64_t second, mac, paired_esp, deadline, fired;
    double slope, offset_us;
    uint16_t revision;
    int span;
} pulse_record_t;
#define RECORDS 32
static pulse_record_t s_records[RECORDS];
static unsigned s_record_count;
static volatile int64_t s_fired;
static esp_timer_handle_t s_edge_timer;
static gptimer_handle_t s_gp;
static mcpwm_timer_handle_t s_pwm;
static mcpwm_oper_handle_t s_oper;
static mcpwm_cmpr_handle_t s_rise, s_fall;
static mcpwm_gen_handle_t s_gen;
static mcpwm_sync_handle_t s_sync;

static bool IRAM_ATTR pwm_edge(mcpwm_cmpr_handle_t comparator, const mcpwm_compare_event_data_t *event, void *ctx)
{
    /* GPIO changed in hardware; this records callback arrival, not edge time. */
    BaseType_t wake=pdFALSE;
    if(s_hz_running) { s_fired=esp_timer_get_time(); vTaskNotifyGiveFromISR(s_hz_task,&wake); }
    return wake==pdTRUE;
}

static void edge_callback(void *arg)
{
    if (!s_hz_running) return;
    s_fired = esp_timer_get_time();
    gpio_set_level(s_gpio, 1);
    xTaskNotifyGive(s_hz_task);
}

static bool IRAM_ATTR gp_edge(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event, void *ctx)
{
    BaseType_t wake = pdFALSE;
    if (s_hz_running) {
        s_fired = esp_timer_get_time();
        gpio_set_level(s_gpio, 1);
        vTaskNotifyGiveFromISR(s_hz_task, &wake);
    }
    return wake == pdTRUE;
}

int strobe_gpio_timing_command(int argc, char **argv)
{
    if(argc==3 && !strcmp(argv[1],"ntp")) {
        if(s_mode!=STROBE_OFF || s_hz_task || !simple_ntp_configure(argv[2])) {
            printf("ERROR stop timing and select non-ntp source before configuring server|IPv4\n");
            return 1;
        }
        simple_ntp_print_status();
        return 0;
    }
    if(argc==3 && !strcmp(argv[1],"monitor")) {
        if(s_mode==STROBE_1HZ) { printf("ERROR use mode off for standalone 1HZ\n"); return 1; }
        if(!strcmp(argv[2],"on")) {
            if(s_mode==STROBE_OFF) { printf("ERROR start camera mode before monitor\n"); return 1; }
            if(!s_load_monitor) {
                /* Suppress camera pulses first; let any <=100-us pulse finish
                 * before the peripheral takes exclusive ownership. */
                s_load_monitor=true;
                vTaskDelay(pdMS_TO_TICKS(10));
                start_hz();
                if(!s_hz_running) { s_load_monitor=false; return 1; }
            }
        } else if(!strcmp(argv[2],"off")) {
            if(s_load_monitor) stop_hz();
            s_load_monitor=false;
        } else { printf("ERROR timing monitor on|off\n"); return 1; }
        printf("timing monitor=%d camera_mode=%s\n",s_load_monitor,strobe_gpio_mode_name(s_mode));
        return 0;
    } else if(argc==3 && !strcmp(argv[1],"pin")) {
        if(s_mode!=STROBE_OFF || s_hz_task) { printf("ERROR stop before pin test\n"); return 1; }
        if(s_gpio<0) return 1;
        gpio_set_direction(s_gpio,GPIO_MODE_INPUT_OUTPUT);
        if(!strcmp(argv[2],"low")) gpio_set_level(s_gpio,0);
        else if(!strcmp(argv[2],"high")) gpio_set_level(s_gpio,1);
        else if(strcmp(argv[2],"read")) return 1;
        printf("timing gpio=%d pad_level=%d\n",s_gpio,gpio_get_level(s_gpio));
        return 0;
    } else if (argc == 4 && !strcmp(argv[1], "config")) {
        if (s_mode != STROBE_OFF || s_hz_task) { printf("ERROR mode must be OFF\n"); return 1; }
        int b=-1, c=-1;
        for(int i=0;i<6;i++) if(!strcmp(argv[2],backend_names[i])) b=i;
        for(int i=0;i<5;i++) if(!strcmp(argv[3],source_names[i])) c=i;
        if(b<0||c<0) { printf("ERROR unknown timing method\n"); return 1; }
        if(!simple_ntp_enable(c==4)) { printf("ERROR configure timing ntp server|IPv4 first\n"); return 1; }
        s_backend=b; s_source=c;
    } else if (argc == 2 && !strcmp(argv[1], "dump")) {
        if (s_hz_task) { printf("ERROR stop before dump\n"); return 1; }
        printf("pulse,second,mac_us,paired_esp_us,deadline_us,fired_us,late_us,rev,slope,offset_us,span_us\n");
        unsigned first=s_record_count>RECORDS?s_record_count-RECORDS:0;
        for(unsigned i=first;i<s_record_count;i++) {
            pulse_record_t *r=&s_records[i%RECORDS];
            printf("P,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%u,%.12f,%.6f,%d\n",
                i,r->second,r->mac,r->paired_esp,r->deadline,r->fired,r->fired-r->deadline,r->revision,r->slope,r->offset_us,r->span);
        }
    }
    printf("timing backend=%s source=%s recorded=%u\n",backend_names[s_backend],source_names[s_source],s_record_count);
    if(s_source==4) simple_ntp_print_status();
    return 0;
}

static void hz_task(void *arg)
{
    clock_model_t frozen = {0};
    s_record_count=0;
    esp_err_t err=ESP_OK;
    if(s_backend==2) {
        esp_timer_create_args_t config={.callback=edge_callback,.name="strobe_edge"};
        err=esp_timer_create(&config,&s_edge_timer);
    } else if(s_backend==3 || s_backend==4) {
        gptimer_config_t config={.clk_src=GPTIMER_CLK_SRC_DEFAULT,.direction=GPTIMER_COUNT_UP,.resolution_hz=1000000,.intr_priority=3};
        err=gptimer_new_timer(&config,&s_gp);
        if(err==ESP_OK) {
            gptimer_event_callbacks_t cb={.on_alarm=gp_edge};
            ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_gp,&cb,NULL));
            ESP_ERROR_CHECK(gptimer_enable(s_gp));
            ESP_ERROR_CHECK(gptimer_start(s_gp));
        }
    } else if(s_backend==5) {
        /* This application exclusively owns group 1, timer 0. The LL counter
         * read is ESP32-S3-specific; do not add another group-1 owner. */
        mcpwm_timer_config_t tc={.group_id=1,.clk_src=MCPWM_TIMER_CLK_SRC_DEFAULT,
            .resolution_hz=1000000,.count_mode=MCPWM_TIMER_COUNT_MODE_UP,.period_ticks=65535,.intr_priority=3};
        ESP_ERROR_CHECK(mcpwm_new_timer(&tc,&s_pwm));
        mcpwm_operator_config_t oc={.group_id=1,.intr_priority=3};
        ESP_ERROR_CHECK(mcpwm_new_operator(&oc,&s_oper));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_oper,s_pwm));
        mcpwm_comparator_config_t cc={.intr_priority=3};
        ESP_ERROR_CHECK(mcpwm_new_comparator(s_oper,&cc,&s_rise));
        ESP_ERROR_CHECK(mcpwm_new_comparator(s_oper,&cc,&s_fall));
        mcpwm_generator_config_t gc={.gen_gpio_num=s_gpio};
        ESP_ERROR_CHECK(mcpwm_new_generator(s_oper,&gc,&s_gen));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_gen,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,s_rise,MCPWM_GEN_ACTION_HIGH)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_gen,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,s_fall,MCPWM_GEN_ACTION_LOW)));
        mcpwm_comparator_event_callbacks_t cb={.on_reach=pwm_edge};
        ESP_ERROR_CHECK(mcpwm_comparator_register_event_callbacks(s_rise,&cb,NULL));
        mcpwm_soft_sync_config_t sc={};
        ESP_ERROR_CHECK(mcpwm_new_soft_sync_src(&sc,&s_sync));
        mcpwm_timer_sync_phase_config_t pc={.sync_src=s_sync,.count_value=0,.direction=MCPWM_TIMER_DIRECTION_UP};
        ESP_ERROR_CHECK(mcpwm_timer_set_phase_on_sync(s_pwm,&pc));
        ESP_ERROR_CHECK(mcpwm_timer_enable(s_pwm));
        ESP_ERROR_CHECK(mcpwm_generator_set_force_level(s_gen,0,true));
    }
    if(err!=ESP_OK) { ESP_LOGE(TAG,"timing backend init: %s",esp_err_to_name(err)); s_hz_running=false; }
    while(s_hz_running) {
        clock_model_t m=clock_model_get();
        bool reference=wifi_is_master();
        int64_t mac=0,timer=0,span=0;
        double hub_us, slope=1, offset=0;
        if(s_source==2) {
            int64_t before=esp_timer_get_time();
            mac=esp_wifi_get_tsf_time(WIFI_IF_STA);
            int64_t after=esp_timer_get_time();
            timer=before+(after-before)/2; span=after-before;
            if(mac<=0) { ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(200)); continue; }
            hub_us=(double)mac;
        } else if(s_source==4) {
            if(!simple_ntp_get(&offset,&m.model_rev) ||
               !sample_mac_clock(&m,true,&mac,&timer,&span)) {
                ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(200)); continue;
            }
            hub_us=(double)mac+offset;
        } else {
            if ((!reference && s_source!=3 && !m.valid) ||
                !sample_mac_clock(&m,reference||s_source==3,&mac,&timer,&span)) {
                ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(200)); continue;
            }
            if(!reference && s_source!=3) {
                if(s_source==1) { if(!frozen.valid) frozen=m; m=frozen; }
                slope=m.slope; offset=m.offset_ps/1e6;
            }
            hub_us=slope*(double)mac+offset;
        }
        int64_t second=(int64_t)floor(hub_us/1e6)+1;
        int64_t deadline=timer+llround(((double)second*1e6-offset)/slope-(double)mac);
        int64_t wait=deadline-esp_timer_get_time();
        if(wait<=0||wait>2000000) { ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50)); continue; }
        s_fired=0;
        if(s_backend==2) {
            ESP_ERROR_CHECK(esp_timer_start_once(s_edge_timer,wait));
            ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2200));
        } else if(s_backend==3 || s_backend==4) {
            uint64_t count;
            int64_t before=esp_timer_get_time();
            ESP_ERROR_CHECK(gptimer_get_raw_count(s_gp,&count));
            int64_t after=esp_timer_get_time();
            int64_t paired=before+(after-before)/2, narrowest=after-before;
            if(s_backend==4) for(int i=0;i<8;i++) {
                uint64_t candidate;
                before=esp_timer_get_time();
                ESP_ERROR_CHECK(gptimer_get_raw_count(s_gp,&candidate));
                after=esp_timer_get_time();
                if(after-before<narrowest) { narrowest=after-before; paired=before+narrowest/2; count=candidate; }
            }
            if(s_backend==4 && narrowest>3) continue;
            gptimer_alarm_config_t alarm={.alarm_count=count+deadline-paired};
            ESP_ERROR_CHECK(gptimer_set_alarm_action(s_gp,&alarm));
            ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2200));
        } else if(s_backend==5) {
            if(wait>10000) ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS((wait-10000)/1000));
            if(!s_hz_running) break;
            ESP_ERROR_CHECK(mcpwm_generator_set_force_level(s_gen,0,true));
            ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_rise,65000));
            ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_fall,65001));
            ESP_ERROR_CHECK(mcpwm_soft_sync_activate(s_sync));
            ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_pwm,MCPWM_TIMER_START_STOP_FULL));
            int64_t paired=0,narrowest=INT64_MAX; uint32_t count=0;
            for(int i=0;i<8;i++) {
                int64_t before=esp_timer_get_time();
                uint32_t candidate=mcpwm_ll_timer_get_count_value(MCPWM_LL_GET_HW(1),0);
                int64_t after=esp_timer_get_time();
                if(after-before<narrowest) { narrowest=after-before; paired=before+narrowest/2; count=candidate; }
            }
            int64_t target=count+deadline-paired;
            if(target<count+300 || target>15000 || narrowest>3) {
                ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(75)); continue;
            }
            ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_rise,(uint32_t)target));
            ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_fall,(uint32_t)target+50000));
            ESP_ERROR_CHECK(mcpwm_generator_set_force_level(s_gen,-1,true));
            ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
        } else {
            if(wait>3000) ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS((wait-2000)/1000));
            while(s_hz_running && esp_timer_get_time()<deadline) { }
            if(s_hz_running) { s_fired=esp_timer_get_time(); gpio_set_level(s_gpio,1); }
        }
        if(!s_hz_running) break;
        if(!s_fired) { ESP_LOGW(TAG,"edge timeout"); continue; }
        s_records[s_record_count++%RECORDS]=(pulse_record_t){.second=second,.mac=mac,.paired_esp=timer,
            .deadline=deadline,.fired=s_fired,.slope=slope,.offset_us=offset,.revision=m.model_rev,.span=(int)span};
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(s_backend==5?75:50));
        gpio_set_level(s_gpio,0);
    }
    if(s_edge_timer) { esp_timer_stop(s_edge_timer); esp_timer_delete(s_edge_timer); s_edge_timer=NULL; }
    if(s_gp) { gptimer_set_alarm_action(s_gp,NULL); gptimer_stop(s_gp); gptimer_disable(s_gp); gptimer_del_timer(s_gp); s_gp=NULL; }
    if(s_pwm) {
        mcpwm_generator_set_force_level(s_gen,0,true);
        mcpwm_timer_start_stop(s_pwm,MCPWM_TIMER_STOP_FULL);
        vTaskDelay(pdMS_TO_TICKS(75));
        mcpwm_timer_disable(s_pwm); mcpwm_del_generator(s_gen);
        mcpwm_del_comparator(s_rise); mcpwm_del_comparator(s_fall);
        mcpwm_del_operator(s_oper); mcpwm_del_timer(s_pwm); mcpwm_del_sync_src(s_sync);
        s_pwm=NULL; configure_pin(s_gpio);
    }
    gpio_set_level(s_gpio,0);
    s_hz_task=NULL;
    vTaskDelete(NULL);
}

static void start_hz(void)
{
    s_hz_running=true;
    if(xTaskCreatePinnedToCore(hz_task,"strobe_hz",6144,NULL,s_backend==1?20:1,&s_hz_task,
        s_backend==1?FTMCS_CAMERA_CORE:FTMCS_CONTROL_CORE)!=pdPASS) s_hz_running=false;
}

static void stop_hz(void)
{
    s_hz_running=false;
    if(s_hz_task) xTaskNotifyGive(s_hz_task);
    /* Do not publish OFF or permit another task until teardown is complete. */
    while(s_hz_task) vTaskDelay(pdMS_TO_TICKS(10));
}

/* ---- public API ---- */

void strobe_gpio_init(int gpio_num)
{
    s_gpio = gpio_num;
    if (CONFIG_FTMCS_IDENTIFY_GPIO >= 0 && CONFIG_FTMCS_IDENTIFY_GPIO != gpio_num) {
        configure_pin(CONFIG_FTMCS_IDENTIFY_GPIO);
    }
    if (gpio_num < 0) {
        ESP_LOGI(TAG, "disabled (gpio<0)");
        return;
    }
    configure_pin(gpio_num);
    ESP_LOGI(TAG, "init on GPIO%d, width=%luus", gpio_num, (unsigned long)s_width_us);
}

strobe_mode_t strobe_gpio_get_mode(void) { return s_mode; }

strobe_mode_t strobe_gpio_set_mode(strobe_mode_t mode)
{
    strobe_mode_t prev = s_mode;
    if (mode == s_mode) return prev;

    /* Tear down the previous mode. Only 1HZ has its own task; camera capture
     * modes are driven externally by strobe_gpio_fire() calls. */
    if (s_hz_task) {
        stop_hz();
    }
    s_load_monitor=false;
    /* Ensure the pin is low before entering the new mode. */
    if (s_gpio >= 0) gpio_set_level(s_gpio, 0);

    s_mode = mode;
    if (mode == STROBE_1HZ) {
        start_hz();
    }
    ESP_LOGI(TAG, "mode: %s -> %s", strobe_gpio_mode_name(prev),
             strobe_gpio_mode_name(mode));
    return prev;
}

void strobe_gpio_fire(void)
{
    if (s_gpio < 0) return;
    /* Every camera capture is observable, including UVC and direct TCP
     * preview. In 1HZ the periodic task exclusively owns the pin. */
    if (s_mode == STROBE_1HZ || s_load_monitor) return;

    gpio_set_level(s_gpio, 1);
    uint64_t end = esp_timer_get_time() + s_width_us;
    while (esp_timer_get_time() < end) {
        /* spin */
    }
    gpio_set_level(s_gpio, 0);
}

void strobe_gpio_set_width_us(uint32_t us)
{
    if (us < 1) us = 1;
    if (us > 100) {
        ESP_LOGW(TAG, "capture pulse capped at 100 us; use identify for visible flashes");
        us = 100;
    }
    s_width_us = us;
    ESP_LOGI(TAG, "width = %lu us", (unsigned long)us);
}

uint32_t strobe_gpio_get_width_us(void) { return s_width_us; }

bool strobe_gpio_identify(void)
{
    const int led_gpio = CONFIG_FTMCS_IDENTIFY_GPIO;
    if (led_gpio < 0) return false;
    /* Preserve support for explicitly configured legacy shared-pin builds. */
    const bool shared = led_gpio == s_gpio;
    strobe_mode_t previous = shared ? strobe_gpio_set_mode(STROBE_OFF) : s_mode;
    for (int i = 0; i < 4; i++) {
        gpio_set_level(led_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(led_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (shared) strobe_gpio_set_mode(previous);
    return true;
}

const char *strobe_gpio_mode_name(strobe_mode_t mode)
{
    switch (mode) {
        case STROBE_LIVE_TRACK:  return "LIVE_TRACK";
        case STROBE_1HZ:         return "1HZ";
        case STROBE_FRAME_SYNC:  return "FRAME_SYNC";
        case STROBE_UVC:         return "UVC";
        case STROBE_OFF:
        default:                 return "OFF";
    }
}
