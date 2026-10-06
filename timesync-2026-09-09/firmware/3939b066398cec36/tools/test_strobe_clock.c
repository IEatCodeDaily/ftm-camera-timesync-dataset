#include <assert.h>
#include <stdio.h>
#include "../main/strobe_clock.h"

int main(void)
{
    assert(strobe_mac_extend(123456U, 120000) == 123456);
    assert(strobe_mac_extend(42U, 0x100000000ULL - 100) == 0x100000000LL + 42);
    assert(strobe_mac_extend(0xfffffff0U, 0x100000000ULL + 100) == 0xfffffff0LL);
    assert(strobe_mac_extend(1000U, 0x300000000ULL + 990) == 0x300000000LL + 1000);
    assert(strobe_mac_extend(990U, 1000) == 990);
    const uint64_t wrap=1ULL<<48;
    assert(strobe_ftm_extend(100,wrap-1000)==wrap+100);
    assert(strobe_ftm_extend(wrap-100,wrap+1000)==wrap-100);
    assert(strobe_ftm_extend(100,3*wrap+1000)==3*wrap+100);
    assert(strobe_ftm_extend(3*wrap+100,3*wrap+1000)==3*wrap+100);
    assert(strobe_ftm_extend(100,1000)==100); /* fresh peer boot */
    uint64_t t1=strobe_ftm_extend(wrap-1000,wrap-2000);
    uint64_t t4=strobe_ftm_extend(3000,t1);
    uint64_t t2=strobe_ftm_extend(5000,2*wrap+10000);
    uint64_t t3=strobe_ftm_extend(7000,t2);
    assert((t4-t1)-(t3-t2)==2000); /* independent local/remote epochs */
    puts("MAC and FTM epoch, rollover, and independent-clock RTT tests passed");
    return 0;
}
