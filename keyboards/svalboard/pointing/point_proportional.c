// SPDX-License-Identifier: GPL-2.0-or-later
#include "point_proportional.h"
#include "report.h"
#include <string.h>

// Keep ample position headroom (over four million reference counts per axis),
// while squared Q8 coordinates and velocity products fit signed 64-bit math.
#define POSITION_MAX (((int64_t)1 << 38) - 1)
#define WHEEL_DENOMINATOR 256000 // Q8 velocity/second, integrated over milliseconds

static uint32_t root(uint64_t n) {
    uint64_t result=0, bit=(uint64_t)1<<62;
    while(bit>n)bit>>=2;
    while(bit) {
        if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}else result>>=1;
        bit>>=2;
    }
    return result;
}
void sval_proportional_discard_output(sval_proportional_t *s,uint32_t now) {
    memset(s->remainder,0,sizeof(s->remainder));
    memset(s->direction,0,sizeof(s->direction));
    s->last_output=now;
}
void sval_proportional_reset(sval_proportional_t *s,uint32_t now) {
    memset(s,0,sizeof(*s));s->last_output=now;
}
void sval_proportional_move(sval_proportional_t *s,int32_t h,int32_t v) {
    const int32_t delta[2]={h,v};
    for(uint8_t axis=0;axis<2;++axis) {
        int64_t position=s->position[axis]+delta[axis];
        s->position[axis]=position>POSITION_MAX?POSITION_MAX:position<-POSITION_MAX?-POSITION_MAX:position;
    }
}
bool sval_proportional_displaced(const sval_proportional_t *s,uint8_t dead_zone) {
    int64_t h=s->position[0]/256,v=s->position[1]/256,d=(int64_t)dead_zone*256;
    return h*h+v*v>d*d;
}
void sval_proportional_output(sval_proportional_t *s,uint16_t horizontal,uint16_t vertical,
    uint8_t dead_zone,uint8_t interval,uint8_t distance_shift,uint8_t axes,
    uint8_t inverted,uint8_t host_divisor,uint16_t wheel_units_per_step,uint32_t now,int16_t out[2]) {
    out[0]=out[1]=0;
    if(s->axes!=axes){sval_proportional_discard_output(s,now);s->axes=axes;}
    uint32_t elapsed=now-s->last_output;s->last_output=now;
    int64_t position[2]={axes&1?s->position[0]/256:0,axes&2?s->position[1]/256:0};
    uint32_t radius=root(position[0]*position[0]+position[1]*position[1]);
    uint32_t dead=(uint32_t)dead_zone*256;
    if(radius<=dead || !horizontal || !vertical || !host_divisor || !wheel_units_per_step) {
        sval_proportional_discard_output(s,now);return;
    }
    const uint16_t distances[2]={horizontal,vertical};
    for(uint8_t axis=0;axis<2;++axis) {
        // Subtract the radial dead zone continuously; there is no speed jump at its edge.
        int64_t effective=position[axis]*(radius-dead)/radius;
        uint32_t distance=((uint32_t)distances[axis]/2)<<distance_shift;
        if(!distance){sval_proportional_discard_output(s,now);return;}
        int64_t velocity=effective*20*wheel_units_per_step/distance;
        if(interval) {
            int64_t maximum=(int64_t)1000*wheel_units_per_step*256/interval;
            if(velocity>maximum)velocity=maximum;
            if(velocity<-maximum)velocity=-maximum;
        }
        velocity/=host_divisor;
        if(inverted&(1<<axis))velocity=-velocity;
        int8_t direction=velocity>0?1:velocity<0?-1:0;
        if(!direction || direction!=s->direction[axis])s->remainder[axis]=0;
        s->direction[axis]=direction;
        if(!direction || !elapsed)continue;
        int64_t magnitude=velocity>0?velocity:-velocity;
        int64_t remaining=s->remainder[axis]>0?s->remainder[axis]:-s->remainder[axis];
        // Only the actual HID field bounds the report. Excess is discarded, never queued.
        // The pre-multiply check also makes an arbitrary timer gap safe from overflow.
        int64_t capacity=(int64_t)MOUSE_REPORT_HV_MAX*WHEEL_DENOMINATOR;
        if(magnitude>(capacity-remaining)/elapsed) {
            out[axis]=direction*MOUSE_REPORT_HV_MAX;s->remainder[axis]=0;
        } else {
            int64_t total=velocity*elapsed+s->remainder[axis];
            out[axis]=total/WHEEL_DENOMINATOR;
            s->remainder[axis]=total%WHEEL_DENOMINATOR;
        }
    }
}
