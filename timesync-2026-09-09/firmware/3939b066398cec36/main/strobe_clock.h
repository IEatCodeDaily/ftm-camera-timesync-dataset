/* Pure clock-domain arithmetic, also exercised by host tests. */
#pragma once
#include <stdint.h>

/* FTM over-the-air timestamps carry 48 picosecond bits (281.474976710656 s).
 * A coarse, recent peer epoch selects the cycle; it does NOT set fine timing. */
static inline uint64_t strobe_ftm_extend(uint64_t raw_ps, uint64_t anchor_ps)
{
    const uint64_t period=1ULL<<48, mask=period-1;
    uint64_t delta=((raw_ps&mask)-(anchor_ps&mask))&mask;
    int64_t signed_delta=delta<(period>>1)?(int64_t)delta:(int64_t)delta-(int64_t)period;
    return (uint64_t)((int64_t)anchor_ps+signed_delta);
}

/* Extend a 32-bit Wi-Fi MAC microsecond counter near a recent 64-bit anchor.
 * The anchor must be within 2^31 us (35.8 minutes) of the counter reading. */
static inline int64_t strobe_mac_extend(uint32_t raw, uint64_t anchor_us)
{
    uint32_t delta = raw - (uint32_t)anchor_us;
    int64_t signed_delta = delta < 0x80000000U ? (int64_t)delta
                                             : (int64_t)delta - 0x100000000LL;
    return (int64_t)anchor_us + signed_delta;
}
