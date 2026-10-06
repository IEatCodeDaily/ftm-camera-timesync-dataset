#include "simple_ntp.h"
#include "strobe_clock.h"
#include "task_cores.h"
#include "esp_timer.h"
#include "esp_private/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define NTP_PORT 7791
#define NTP_MAGIC 0x3150544eU
/* Private protocol between identical ESP32 little-endian builds. */
typedef struct {
    uint32_t magic, seq;
    int64_t t1, t2, t3;
} ntp_packet_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static volatile bool s_running;
static bool s_server, s_configured;
static uint32_t s_ip;
static double s_offset;
static int64_t s_updated, s_rtt;
static uint32_t s_accepted, s_rejected;

static int64_t mac_now(void)
{
    return strobe_mac_extend(esp_wifi_internal_get_mac_clock_time(),
                             (uint64_t)esp_timer_get_time());
}

static void ntp_task(void *unused)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) goto done;
    struct timeval timeout = {.tv_sec=0, .tv_usec=250000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_port=htons(NTP_PORT)};
    addr.sin_addr.s_addr = s_server ? htonl(INADDR_ANY) : s_ip;
    if (s_server) {
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto close_done;
    } else if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto close_done;
    uint32_t seq=0;
    while (s_running) {
        ntp_packet_t p;
        if (s_server) {
            struct sockaddr_in peer;
            socklen_t len=sizeof(peer);
            int n=recvfrom(fd,&p,sizeof(p),0,(struct sockaddr *)&peer,&len);
            int64_t t2=mac_now();
            if (n != sizeof(p) || p.magic != NTP_MAGIC || p.t2 || p.t3) continue;
            p.t2=t2;
            p.t3=mac_now();
            sendto(fd,&p,sizeof(p),0,(struct sockaddr *)&peer,len);
        } else {
            int64_t cycle=esp_timer_get_time();
            p=(ntp_packet_t){.magic=NTP_MAGIC,.seq=++seq};
            p.t1=mac_now();
            int64_t t1=p.t1;
            int sent=send(fd,&p,sizeof(p),0);
            int n=sent==sizeof(p)?recv(fd,&p,sizeof(p),0):-1;
            int64_t t4=mac_now(), rtt=0;
            double offset=0;
            bool ok=n==sizeof(p) && p.magic==NTP_MAGIC && p.seq==seq && p.t1==t1
                 && simple_ntp_estimate(t1,p.t2,p.t3,t4,&offset,&rtt);
            portENTER_CRITICAL(&s_lock);
            if (ok) {
                s_offset=offset; s_rtt=rtt; s_updated=esp_timer_get_time(); s_accepted++;
            } else s_rejected++;
            portEXIT_CRITICAL(&s_lock);
            while(s_running && esp_timer_get_time()-cycle<1000000)
                vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
close_done:
    close(fd);
done:
    s_running=false;
    s_task=NULL;
    vTaskDelete(NULL);
}

bool simple_ntp_configure(const char *endpoint)
{
    if (s_task) return false;
    struct in_addr ip;
    bool server=!strcmp(endpoint,"server");
    if (!server && !inet_aton(endpoint,&ip)) return false;
    s_server=server; s_ip=server?0:ip.s_addr; s_configured=true;
    return true;
}

bool simple_ntp_enable(bool enabled)
{
    if (!enabled) {
        s_running=false;
        while(s_task) vTaskDelay(pdMS_TO_TICKS(10));
        return true;
    }
    if (s_task) return true;
    if (!s_configured) return false;
    s_offset=0; s_updated=0; s_rtt=0; s_accepted=0; s_rejected=0;
    s_running=true;
    if(xTaskCreatePinnedToCore(ntp_task,"simple_ntp",4096,NULL,4,&s_task,
                              FTMCS_CONTROL_CORE)!=pdPASS) {
        s_running=false; s_task=NULL; return false;
    }
    return true;
}

bool simple_ntp_get(double *offset_us, uint16_t *revision)
{
    portENTER_CRITICAL(&s_lock);
    bool ready=s_running && (s_server || (s_accepted && esp_timer_get_time()-s_updated<3000000));
    *offset_us=s_server?0:s_offset; *revision=(uint16_t)s_accepted;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

void simple_ntp_print_status(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t accepted=s_accepted,rejected=s_rejected;
    int64_t rtt=s_rtt,age=s_updated?esp_timer_get_time()-s_updated:-1;
    double offset=s_offset;
    portEXIT_CRITICAL(&s_lock);
    printf("ntp running=%d server=%d poll_ms=1000 accepted=%" PRIu32 " rejected=%" PRIu32
           " offset_us=%.3f rtt_us=%" PRId64 " age_us=%" PRId64 "\n",
           s_running,s_server,accepted,rejected,offset,rtt,age);
}
