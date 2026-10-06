#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Experimental software four-timestamp UDP baseline, not RFC NTP/SNTP.
 * All times are extended local Wi-Fi MAC microseconds. No frequency fit. */
bool simple_ntp_configure(const char *endpoint); /* "server" or IPv4 */
bool simple_ntp_enable(bool enabled);
bool simple_ntp_get(double *offset_us, uint16_t *revision);
void simple_ntp_print_status(void);

static inline bool simple_ntp_estimate(int64_t t1, int64_t t2, int64_t t3,
                                       int64_t t4, double *offset, int64_t *rtt)
{
    if (t4 < t1 || t3 < t2) return false;
    *rtt = (t4-t1)-(t3-t2);
    if (*rtt < 0 || *rtt > 250000) return false;
    *offset = ((double)(t2-t1)+(double)(t3-t4))/2.0;
    return true;
}
