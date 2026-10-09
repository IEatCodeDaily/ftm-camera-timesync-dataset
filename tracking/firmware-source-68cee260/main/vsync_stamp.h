/*
 * vsync_stamp.h -- hardware-latched camera VSYNC timestamps (MCPWM capture).
 */
#ifndef VSYNC_STAMP_H
#define VSYNC_STAMP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool on;
    uint32_t hits, misses, rise, fall;
    /* sw_lag: the old software stamp (cam_task) minus the hardware edge.
     * isr_lag: the driver's VSYNC interrupt stamp minus the hardware edge. */
    int32_t sw_lag_min_us, sw_lag_max_us, isr_lag_min_us, isr_lag_max_us;
    int64_t sw_lag_sum_us;
} vsync_stamp_stats_t;

/* Route the camera's VSYNC pin to MCPWM group 0 capture and make cam_hal
 * stamp frames with the latched edge. Off: the driver's software stamp. */
bool vsync_stamp_set(bool on);
void vsync_stamp_stats(vsync_stamp_stats_t *out, bool reset);

#ifdef __cplusplus
}
#endif
#endif /* VSYNC_STAMP_H */
