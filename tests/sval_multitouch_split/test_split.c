#define SVAL_MT_HOST_TEST
#define SVAL_MT_SPLIT
#include "../../keyboards/svalboard/azoteq/multitouch.c"
#include "../../keyboards/svalboard/azoteq/touch_gestures.c"
#include <stdio.h>
uint32_t sval_mt_rp_abort_source;
static int emits, activity_calls;
void tg_platform_emit(uint16_t keycode) { (void)keycode; emits++; }
void tg_storage_read(uint8_t block[TG_BLOCK_SIZE]) { memset(block, 0xFF, TG_BLOCK_SIZE); }
void tg_storage_write(const uint8_t block[TG_BLOCK_SIZE]) { (void)block; }
void sval_mt_contact_activity(bool left, uint16_t movement) { (void)left; assert(movement); activity_calls++; }
// One local sensor sample: n fingers at x (raw counts), all on this pad.
static void local_sample(uint8_t n, uint16_t x) {
 uint8_t raw[45]={0};
 raw[5]=n;
 for (uint8_t i=0;i<n;i++) { uint8_t *p=raw+10+7*i; p[0]=(x+200*i)>>8; p[1]=(x+200*i)&255; p[2]=0; p[3]=100; p[4]=0; p[5]=40; }
 sval_mt_sample(raw,true);
}
static void lease(uint8_t *cmd, uint8_t op, uint32_t id) {
 memset(cmd,0,64); put32(cmd+1,id); cmd[6]=op; cmd[7]=op==1?2:0; put16(cmd+8,token);
 sval_mt_command(cmd,64);
}
int main(void) {
 uint8_t raw[45]={0},request=1;
 tg_load(); assert(!tg_running() && tg_config.owners==TG_OPT_SPAN);
 master=false; clock_ms=10; sval_mt_init();
 raw[5]=1; raw[10]=1; raw[11]=2; raw[14]=0; raw[15]=40;
 sval_mt_sample(raw,true);
 assert(snapshot_valid && !count && remote_snapshot[1]==0 && remote_snapshot[3]==1);
 clock_ms=15; mt_rpc(1,&request,60,rpc_data);
 assert(rpc_data[0]==0xA7 && rpc_data[1] && rpc_data[2]==5);
 assert(!memcmp(rpc_data+4,remote_snapshot,56));
 // Master forwards a coherent left frame once, not each duplicate poll.
 master=true; connected=true; client=123; clock_ms=20;
 poll_remote(); assert(count==1 && queue[0][1]==0 && queue[0][3]==1);
 clock_ms=30; poll_remote(); assert(count==1);
 // Failed link emits one invalidation after the freshness deadline.
 connected=false; clock_ms=140; poll_remote(); assert(count==2 && queue[1][2]==0x20 && !queue[1][3]);
 clock_ms=160; poll_remote(); assert(count==2);
 // Recovery is discontinuous even if the source sequence is identical.
 connected=true; clock_ms=180; poll_remote(); assert(count==3 && (queue[2][2]&16));
 // Stale remote snapshot cannot masquerade as new contacts.
 rpc_data[2]=101; clock_ms=300; poll_remote(); assert(count==4 && queue[3][2]==0x20);
 // Reversed/malformed hand is refused.
 remote_seen=false; rpc_data[2]=0; rpc_data[5]=1; clock_ms=320; poll_remote(); assert(count==4);
 // Update activity never starts another split transaction.
 busy=true; clock_ms=400; poll_remote(); assert(count==4);
 // Pair firmware refuses old single-pad injection ownership.
 uint8_t cmd[64]={0}; put32(cmd+1,123); cmd[6]=1; cmd[7]=1;
 sval_mt_command(cmd,64); assert(cmd[7]==2);
 // Explicit pair capture suppresses both reports only while the lease lives.
 memset(cmd,0,sizeof(cmd)); put32(cmd+1,123); cmd[6]=1; cmd[7]=2;
 sval_mt_command(cmd,64); assert(!cmd[7] && (cmd[9]&8));
 assert(sval_mt_suppress_pair_mouse());
 clock_ms+=500; assert(!sval_mt_suppress_pair_mouse());
 sval_mt_task(); assert(!pair_capture && !client);
 // Queue overload is explicit and bounded.
 uint8_t f[56]={1}; count=8; head=0; queue_remote(f);
 assert(count==8 && dropped==1 && (queue[0][2]&16));
 // ---- unified mode handoff, scroll owner, rebind, claim flag, auto mouse
 count=0; head=0; connected=false; busy=false; clock_ms+=1000;
 uint8_t c2[64]; int16_t h, v;
 lease(c2,1,123); assert(!c2[7] && (c2[9]&8) && !(c2[9]&0x30));
 h=5; v=7; assert(sval_mt_chip_filter(0,&h,&v)==2); // unified, scroll owner Windows: nothing passes
 tg_config.owners|=TG_OWN_SCROLL_CHIP;
 lease(c2,2,123); assert(!c2[7] && (c2[9]&0x10));
 h=5; v=7; assert(sval_mt_chip_filter(1,&h,&v)==1 && h==5 && v==7); // the chip wheel passes
 // One finger moving: frames queue, and auto mouse is fed from contact movement.
 activity_calls=0;
 local_sample(1,500); clock_ms+=10; local_sample(1,560);
 assert(count==2 && activity_calls==1 && queue[1][3]==1);
 // A renew from a fresh wrapper ID carries the lease only with the old ID.
 lease(c2,2,456); assert(c2[7]==3 && client==123);
 memset(c2,0,64); put32(c2+1,456); c2[6]=2; put16(c2+8,token); put32(c2+10,123);
 sval_mt_command(c2,64); assert(!c2[7] && client==456 && sval_mt_suppress_pair_mouse());
 // STOP mid-touch: chip output stays off until both pads are all-up.
 lease(c2,3,456); assert(!c2[7] && !client);
 assert(sval_mt_lease_mode()==0x80);
 h=1; v=1; assert(sval_mt_chip_filter(0,&h,&v)==2);
 sval_mt_task(); assert(pair_recovering); // the finger is still down
 clock_ms+=10; local_sample(0,0); sval_mt_task(); // local all-up; the far half is disconnected
 assert(!pair_recovering && sval_mt_chip_filter(0,&h,&v)==0);
 // Lease expiry mid-touch is the same handoff, and the far pad's own all-up counts.
 lease(c2,1,456); local_sample(1,500); connected=true;
 rpc_data[0]=0xA7; rpc_data[1]=1; rpc_data[2]=0; rpc_data[3]=0; memset(rpc_data+4,0,56);
 rpc_data[4]=1; rpc_data[5]=0; rpc_data[7]=1; rpc_data[8]=1; rpc_data[20]=1; // far pad: one finger
 clock_ms+=20; poll_remote(); assert(!far_up);
 clock_ms+=500; sval_mt_task(); assert(!client && pair_recovering);
 clock_ms+=10; local_sample(0,0); sval_mt_task(); assert(pair_recovering); // far finger still down
 rpc_data[7]=0; rpc_data[8]=2; clock_ms+=10; poll_remote(); assert(far_up);
 sval_mt_task(); assert(!pair_recovering);
 // A firmware swipe claims: queued frames carry 0x40 and the chip is held back until all-up.
 tg_config.owners=TG_OPT_SPAN|TG_OWN_SWIPE_FW; connected=false;
 lease(c2,1,456); count=0; head=0; emits=0;
 for (int k=0;k<20;k++) { local_sample(3,1400-k*60); sval_mt_task(); clock_ms+=10; }
 assert(emits==1 && tg_claimed());
 local_sample(3,260); assert(count && (queue[(head+count-1)%QUEUE_SIZE][2]&0x40));
 lease(c2,3,456); h=3; v=3; assert(sval_mt_chip_filter(0,&h,&v)==2); // normal mode, claimed
 local_sample(0,0); sval_mt_task(); assert(tg_claimed()); // dropped contacts are held for TG_DROP_GRACE_MS
 clock_ms+=TG_DROP_GRACE_MS+1; sval_mt_task(); assert(!tg_claimed() && sval_mt_chip_filter(0,&h,&v)==0);
 // Normal mode with no host: the recognizer still runs and polls the far half.
 assert(!client && sval_mt_lease_mode()==0);
 remote_poll=clock_ms-20; connected=true; rpc_data[8]=3; rpc_data[7]=0;
 sval_mt_task(); assert(remote_poll==clock_ms && count==0);
 puts("split multitouch: coherent snapshot, deduplication, staleness, recovery, role validation, updater exclusion, relay refusal, overflow, chip wheel pass-through, lease rebind, all-up handoff (local and far pad), claim flag and auto mouse passed");
}
