#include <assert.h>
#include <stdio.h>
#include "../main/simple_ntp.h"
#include "../main/strobe_clock.h"
int main(void)
{
    double offset; int64_t rtt;
    /* Equal 100-us paths, 50-us processing, server 5 ms ahead. */
    assert(simple_ntp_estimate(1000,6100,6150,1250,&offset,&rtt));
    assert(offset==5000 && rtt==200);
    /* 100/300-us path asymmetry biases offset by half the difference. */
    assert(simple_ntp_estimate(1000,6100,6150,1450,&offset,&rtt));
    assert(offset==4900 && rtt==400);
    /* Independent large epochs and a local MAC rollover. */
    int64_t t1=0x1fffffff0LL, t4=strobe_mac_extend(234, (uint64_t)t1);
    assert(simple_ntp_estimate(t1,t1-20000000+100,t1-20000000+150,t4,&offset,&rtt));
    assert(offset==-20000000 && rtt==200);
    assert(!simple_ntp_estimate(1000,100,400,1250,&offset,&rtt));
    assert(!simple_ntp_estimate(1000,100,50,1250,&offset,&rtt));
    assert(!simple_ntp_estimate(1000,100,150,999,&offset,&rtt));
    assert(!simple_ntp_estimate(1000,100,150,300000,&offset,&rtt));
    puts("four-timestamp offset, asymmetry, rollover and rejection tests passed");
}
