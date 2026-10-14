#define SVAL_MT_HOST_TEST
#define SVAL_MT_SPLIT
#include "../../keyboards/svalboard/azoteq/multitouch.c"
#include <stdio.h>
int main(void) {
 uint8_t raw[45]={0},request=1;
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
 connected=false; clock_ms=140; poll_remote(); assert(count==2 && queue[1][2]==4 && !queue[1][3]);
 clock_ms=160; poll_remote(); assert(count==2);
 // Recovery is discontinuous even if the source sequence is identical.
 connected=true; clock_ms=180; poll_remote(); assert(count==3 && (queue[2][2]&16));
 // Stale remote snapshot cannot masquerade as new contacts.
 rpc_data[2]=101; clock_ms=300; poll_remote(); assert(count==4 && queue[3][2]==4);
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
 puts("split multitouch: coherent snapshot, deduplication, staleness, recovery, role validation, updater exclusion, relay refusal and overflow passed");
}
