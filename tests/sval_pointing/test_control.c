#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include "point_control.c"
static uint32_t          clock_ms;
static int               configured = USB_DEVICE_STATE_CONFIGURED;
static report_keyboard_t last;
uint32_t                 timer_read32(void) {
                    return clock_ms;
}
bool is_keyboard_master(void) {
    return true;
}
bool sval_other_half_connected(void) {
    return false;
}
int usb_device_state_get_configure_state(void) {
    return configured;
}
usb_hid_protocol_t usb_device_state_get_protocol(void) {
    return USB_PROTOCOL_REPORT;
}
void sval_point_transport_install(void) {}
bool sval_point_transport_keyboard(report_keyboard_t *r) {
    last = *r;
    return true;
}
bool sval_point_transport_nkro(report_nkro_t *r) {
    (void)r;
    return true;
}
bool sval_point_transport_extra(report_extra_t *r) {
    (void)r;
    return true;
}
static void event(uint8_t pos, uint16_t code, bool down) {
    keyrecord_t r = {.event = {.key = {.row = pos / 6, .col = pos % 6}, .pressed = down}};
    assert(!sval_point_process(code, &r));
}
static void motion(int x, int y, uint8_t axes) {
    ++clock_ms;
    sval_point_buffer(0, x, y, 800, 1);
    sval_point_finish_interval(axes);
}
int main(void) {
    event(0, SV_POINT_L_VOLUME_TOGGLE, true);
    event(0, SV_POINT_L_VOLUME_TOGGLE, false);
    assert(pointers[0].control == 1);
    event(1, SV_POINT_L_TABS_HOLD, true);
    event(2, SV_POINT_L_ARROWS_HOLD, true);
    // Release resolves by physical identity even after the keymap changes.
    event(1, 4, false);
    assert(pointers[0].control == 2);
    event(2, 4, false);
    assert(pointers[0].control == 1);
    // Suppressed travel must not later be reinterpreted as steps.
    motion(0, -1000, SVAL_AXIS_H);
    assert(!pending_count(0));
    motion(0, -1, SVAL_AXIS_V);
    assert(!pending_count(0));
    motion(0, -63, SVAL_AXIS_V);
    assert(pending_count(0) && pointers[0].pending[1].direction == 1);
    sval_point_policy_changed(true);
    assert(!pending_count(0));
    event(3, SV_POINT_L_ARROWS_HOLD, true);
    uint32_t before = recognized;
    motion(64, -64, SVAL_AXIS_BOTH);
    assert(recognized == before + 2);
    event(3, 4, false);
    assert(!pending_count(0));
    // Buffered motion is invalidated on effective control transition.
    sval_point_buffer(0, 0, -500, 800, 1);
    event(4, SV_POINT_L_TASKS_HOLD, true);
    assert(pointers[0].motion_count == 0);
    motion(96, 0, SVAL_AXIS_BOTH);
    sval_point_task();
    ++clock_ms;
    sval_point_task();
    assert(sval_point_output_session() && last.mods == 4);
    event(5, KC_ESC, true);
    event(5, KC_ESC, false);
    assert(!sval_point_output_session() && pointers[0].inhibited);
    event(4, 4, false);
    assert(pointers[0].control == 1 && !pointers[0].inhibited);
    // Suspend clears activations but consumes their eventual releases.
    event(6, SV_POINT_L_TABS_HOLD, true);
    configured = USB_DEVICE_STATE_SUSPEND;
    sval_point_task();
    assert(!sval_point_active(0));
    event(6, 4, false);
    // Different axis distances flow through the actual controller, not just storage.
    configured         = USB_DEVICE_STATE_CONFIGURED;
    uint8_t packet[32] = {0xDF, sval_cmd_point_tools_get};
    sval_point_command(packet, 26);
    uint8_t lo = packet[4], hi = packet[5];
    memmove(packet + 5, packet + 6, 19);
    packet[1] = sval_cmd_point_tools_set;
    packet[2] = 5;
    packet[3] = lo;
    packet[4] = hi;
    packet[5] = (packet[5] & 63) | ((45 & 3) << 6);
    packet[6] = (packet[6] & 0xC0) | (45 >> 2) | (1 << 5);
    sval_point_command(packet, 26);
    assert(packet[2] == 0);
    event(7, SV_POINT_L_ARROWS_HOLD, true);
    before = recognized;
    motion(64, -32, SVAL_AXIS_BOTH);
    assert(recognized == before + 2);
    event(7, 4, false);
    puts("controller: holds, remapped releases, masks, free diagonals, generation, session, suspend passed");
}
