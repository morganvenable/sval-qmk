#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include "point_control.c"
static uint32_t clock_ms;
static report_keyboard_t last_down;
static unsigned presses;
uint32_t timer_read32(void) { return clock_ms; }
bool is_keyboard_master(void) { return true; }
bool sval_other_half_connected(void) { return false; }
int usb_device_state_get_configure_state(void) { return USB_DEVICE_STATE_CONFIGURED; }
usb_hid_protocol_t usb_device_state_get_protocol(void) { return USB_PROTOCOL_REPORT; }
void sval_point_transport_install(void) {}
bool sval_point_transport_keyboard(report_keyboard_t *r) { if(r->keys[0]){last_down=*r;presses++;}return true; }
bool sval_point_transport_nkro(report_nkro_t *r) { (void)r;return true; }
bool sval_point_transport_extra(report_extra_t *r) { (void)r;return true; }
static void event(uint16_t code,bool down) {
    keyrecord_t r={.event={.key={.row=0,.col=0},.pressed=down}};
    assert(!sval_point_process(code,&r));
}
static void tick(void) { for(int i=0;i<8;++i){++clock_ms;sval_point_task();} }
static void configure(uint8_t action,uint8_t reverse) {
    sval_point_stop();tick();
    uint8_t p[32]={0xDF,sval_cmd_point_tools_get};sval_point_command(p,26);
    uint8_t lo=p[4],hi=p[5];memmove(p+5,p+6,19);p[1]=sval_cmd_point_tools_set;p[2]=5;p[3]=lo;p[4]=hi;
    p[21]=(p[21]&0xE0)|action|(reverse<<3);
    sval_point_command(p,26);assert(!p[2]);
    sval_point_config_reload();assert(sval_point_tool(1)==action);
    event(SV_POINT_L_VOLUME_HOLD,true);
}
static void motion(int x,int y,uint8_t axes) {
    ++clock_ms;sval_point_buffer(0,x,y,800,1);sval_point_finish_interval(axes);tick();
}
int main(void) {
    configure(SVAL_TOOL_ZOOM,0);
    motion(0,-64,SVAL_AXIS_BOTH);assert(last_down.keys[0]==KC_EQUAL&&last_down.mods==1);
    motion(0,64,SVAL_AXIS_BOTH);assert(last_down.keys[0]==KC_MINUS&&last_down.mods==1);
    unsigned before=presses;motion(640,0,SVAL_AXIS_BOTH);assert(presses==before);
    motion(0,-640,SVAL_AXIS_H);assert(presses==before); // shared axis policy still wins
    event(SV_POINT_L_VOLUME_HOLD,false);
    configure(SVAL_TOOL_ZOOM,2);motion(0,-64,SVAL_AXIS_V);assert(last_down.keys[0]==KC_MINUS);
    event(SV_POINT_L_VOLUME_HOLD,false);
    configure(SVAL_TOOL_HISTORY,0);
    motion(-64,0,SVAL_AXIS_BOTH);assert(last_down.keys[0]==KC_Z&&last_down.mods==1);
    motion(64,0,SVAL_AXIS_BOTH);assert(last_down.keys[0]==KC_Z&&last_down.mods==3);
    before=presses;motion(0,-640,SVAL_AXIS_BOTH);assert(presses==before);
    // A stopped ball does not replay surplus undo/redo actions.
    sval_point_buffer(0,0,0,800,1);tick();assert(presses==before);
    event(SV_POINT_L_VOLUME_HOLD,false);
    configure(SVAL_TOOL_HISTORY_Y,1);motion(-64,0,SVAL_AXIS_H);assert(last_down.keys[0]==KC_Y&&last_down.mods==1);
    event(SV_POINT_L_VOLUME_HOLD,false);
    // Session lifecycle follows the selected tool, not the original slot label.
    configure(SVAL_TOOL_TASKS,0);motion(64,0,SVAL_AXIS_H);assert(sval_point_output_session());
    event(SV_POINT_L_VOLUME_HOLD,false);tick();assert(!sval_point_output_session());
    puts("tools: zoom, both redo shortcuts, direction, axes, no replay, persistence, remapped task release passed");
}
