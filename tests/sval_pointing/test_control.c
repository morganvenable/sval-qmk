#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include "point_control.c"
static uint32_t          clock_ms;
static int               configured = USB_DEVICE_STATE_CONFIGURED;
static report_keyboard_t last;
static unsigned          escapes;
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
    if (r->keys[0] == KC_ESC) ++escapes;
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
    sval_point_buffer(0, x, y, 800);
    sval_point_finish_interval(axes);
}
static void both(int lx, int ly, int rx, int ry) {
    ++clock_ms;
    sval_point_buffer(0, lx, ly, 800);
    sval_point_buffer(1, rx, ry, 800);
    sval_point_finish_interval(SVAL_AXIS_BOTH);
}
int main(void) {
    event(0, SV_POINT_TOOL1_TOGGLE, true);
    event(0, SV_POINT_TOOL1_TOGGLE, false);
    assert(pointers[0].control == 1);
    event(1, SV_POINT_TOOL3_HOLD, true);
    event(2, SV_POINT_TOOL2_HOLD, true);
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
    event(3, SV_POINT_TOOL2_HOLD, true);
    uint32_t before = recognized;
    motion(160, -160, SVAL_AXIS_BOTH); // one Arrows step each way: 2.5 x 64 counts
    assert(recognized == before + 2);
    event(3, 4, false);
    assert(!pending_count(0));
    // Buffered motion is invalidated on effective control transition.
    sval_point_buffer(0, 0, -500, 800);
    event(4, SV_POINT_TOOL4_HOLD, true);
    assert(pointers[0].motion_count == 0);
    motion(320, 0, SVAL_AXIS_BOTH); // one Tasks step
    sval_point_task();
    ++clock_ms;
    sval_point_task();
    assert(sval_point_output_session() && last.mods == 4);
    event(5, KC_ESC, true);
    event(5, KC_ESC, false);
    assert(!sval_point_output_session() && pointers[0].inhibited);
    event(4, 4, false);
    assert(pointers[0].control == 1 && !pointers[0].inhibited);
    // Holding never times out: a held Tasks session stays open with no step for a minute,
    // and only the release ends it, accepting (no Escape).
    event(4, SV_POINT_TOOL4_HOLD, true);
    motion(320, 0, SVAL_AXIS_BOTH);
    for (int i = 0; i < 60000; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(sval_point_output_session() && last.mods == 4 && !pointers[0].inhibited);
    unsigned escaped = escapes;
    event(4, 4, false);
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(!sval_point_output_session() && !last.mods && escapes == escaped);
    // A toggled session has a safety end: 10 s with no step cancels it (Escape), so Alt
    // never stays down on the host with nothing held on the board. The toggle turns off.
    event(4, SV_POINT_TOOL4_TOGGLE, true);
    event(4, 4, false);
    motion(320, 0, SVAL_AXIS_BOTH);
    sval_point_task();
    ++clock_ms;
    sval_point_task();
    assert(sval_point_output_session());
    for (int i = 0; i < 9900; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(sval_point_output_session());
    for (int i = 0; i < 200; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(!sval_point_output_session() && escapes == escaped + 1 && !last.mods && toggled == NONE);
    // A hold over an open toggled session takes it over: no timeout while held, and the
    // 10 s start again from the release, with the toggle still on.
    event(4, SV_POINT_TOOL4_TOGGLE, true);
    event(4, 4, false);
    motion(320, 0, SVAL_AXIS_BOTH);
    sval_point_task();
    assert(sval_point_output_session());
    event(5, SV_POINT_TOOL4_HOLD, true);
    for (int i = 0; i < 30000; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(sval_point_output_session());
    event(5, 4, false); // the toggle still runs Tasks: the session stays open
    for (int i = 0; i < 9900; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(sval_point_output_session());
    for (int i = 0; i < 200; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    assert(!sval_point_output_session() && toggled == NONE);
    event(4, SV_POINT_TOOL1_TOGGLE, true);
    event(4, 4, false);
    assert(pointers[0].control == 1);
    // Suspend clears activations but consumes their eventual releases.
    event(6, SV_POINT_TOOL3_HOLD, true);
    configured = USB_DEVICE_STATE_SUSPEND;
    sval_point_task();
    assert(!sval_point_active(0));
    event(6, 4, false);
    // Different axis distances flow through the actual controller, not just storage.
    configured         = USB_DEVICE_STATE_CONFIGURED;
    uint8_t packet[32] = {0xDF, sval_cmd_point_tools_get};
    sval_point_command(packet, 27);
    uint8_t lo = packet[4], hi = packet[5];
    memmove(packet + 5, packet + 6, 19);
    packet[1] = sval_cmd_point_tools_set;
    packet[2] = 5;
    packet[3] = lo;
    packet[4] = hi;
    packet[5] = (packet[5] & 63) | ((45 & 3) << 6);
    packet[6] = (packet[6] & 0xC0) | (45 >> 2) | (1 << 5);
    sval_point_command(packet, 27);
    assert(packet[2] == 0);
    event(7, SV_POINT_TOOL2_HOLD, true);
    before = recognized;
    motion(160, -80, SVAL_AXIS_BOTH); // Arrows: 2.5 x the 64 across and 32 up
    assert(recognized == before + 2);
    event(7, 4, false);

    // Tools aren't per side: one set of keys, and every pointer runs what they resolve to.
    sval_point_stop();
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    event(8, SV_POINT_TOOL2_HOLD, true);
    assert(pointers[0].control == 2 && pointers[1].control == 2);
    before = recognized;
    both(160, 0, 0, -80); // slot 2 (Arrows) now steps every 160 counts across, 80 up (set above)
    assert(recognized == before + 2 && pointers[0].pending[0].count == 1 && pointers[1].pending[1].count == 1);
    event(8, 4, false);
    assert(!sval_point_active(0) && !sval_point_active(1) && !pending_count(0) && !pending_count(1));
    // Precedence: the newest held key wins over the toggle, on both pointers alike.
    event(9, SV_POINT_TOOL1_TOGGLE, true);
    event(9, SV_POINT_TOOL1_TOGGLE, false);
    event(10, SV_POINT_TOOL3_HOLD, true);
    event(11, SV_POINT_TOOL2_HOLD, true);
    assert(pointers[0].control == 2 && pointers[1].control == 2);
    event(10, 4, false); // releasing a replaced hold doesn't disturb the newer one
    assert(pointers[0].control == 2 && pointers[1].control == 2);
    event(10, SV_POINT_TOOL3_HOLD, true);
    assert(pointers[0].control == 3 && pointers[1].control == 3);
    event(10, 4, false); // the older key still held (11) doesn't come back: the toggle runs
    assert(pointers[0].control == 1 && pointers[1].control == 1);
    event(11, 4, false);
    assert(pointers[0].control == 1 && pointers[1].control == 1);
    event(9, SV_POINT_TOOL1_TOGGLE, true);
    event(9, SV_POINT_TOOL1_TOGGLE, false);
    assert(!sval_point_active(0) && !sval_point_active(1));
    // Two holds, no toggle: the newest wins; releasing it runs nothing, though the older key
    // is still down, until a tool key is pressed again.
    event(10, SV_POINT_TOOL1_HOLD, true);
    event(11, SV_POINT_TOOL2_HOLD, true);
    assert(pointers[0].control == 2 && pointers[1].control == 2);
    event(11, 4, false);
    assert(!sval_point_active(0) && !sval_point_active(1));
    before = recognized;
    both(0, -640, 640, 0);
    assert(recognized == before && !pending_count(0) && !pending_count(1));
    event(10, 4, false); // the older key's release changes nothing
    assert(!sval_point_active(0) && !sval_point_active(1));
    event(10, SV_POINT_TOOL1_HOLD, true);
    event(11, SV_POINT_TOOL2_HOLD, true);
    event(11, 4, false);
    event(12, SV_POINT_TOOL3_HOLD, true); // a new press starts its tool at once
    assert(pointers[0].control == 3 && pointers[1].control == 3);
    event(12, 4, false);
    event(10, 4, false);
    assert(!sval_point_active(0) && !sval_point_active(1));
    // One toggle: another slot replaces it, the same slot clears it.
    event(9, SV_POINT_TOOL2_TOGGLE, true);
    event(9, SV_POINT_TOOL2_TOGGLE, false);
    assert(pointers[0].control == 2 && pointers[1].control == 2);
    event(9, SV_POINT_TOOL2_TOGGLE, true);
    event(9, SV_POINT_TOOL2_TOGGLE, false);
    assert(!sval_point_active(0) && !sval_point_active(1));
    // Toggle, then Stop all ends both pointers, holds included; held keys release quietly.
    event(9, SV_POINT_TOOL1_TOGGLE, true);
    event(9, SV_POINT_TOOL1_TOGGLE, false);
    event(10, SV_POINT_TOOL3_HOLD, true);
    both(0, -64, 0, -64);
    event(12, SV_POINT_STOP_ALL, true);
    assert(!sval_point_active(0) && !sval_point_active(1) && !pending_count(0) && !pending_count(1));
    event(12, SV_POINT_STOP_ALL, false);
    event(10, 4, false);
    assert(!sval_point_active(0) && !sval_point_active(1));
    // Retired and reserved codes are swallowed and do nothing, press and release.
    const uint16_t retired[] = {SV_POINT_FIRST + 8, SV_POINT_FIRST + 15, SV_POINT_FIRST + 0x12, SV_POINT_FIRST + 0x13, SV_POINT_FIRST + 0x14, SV_POINT_RESERVED_LAST};
    for (unsigned i = 0; i < sizeof(retired) / sizeof(retired[0]); ++i) {
        event(13, retired[i], true);
        assert(!sval_point_active(0) && !sval_point_active(1));
        event(13, KC_Y, false); // the release is swallowed too
    }
    // STATUS: both pointers report the tool; byte 8 (retired distance shift) reads 1.
    event(14, SV_POINT_TOOL3_HOLD, true);
    uint8_t status[32] = {0xDF, sval_cmd_point_tools_status};
    sval_point_command(status, 32);
    assert(status[2] == 2 && status[3] == 3 && status[4] == 3 && status[8] == 1);
    event(14, 4, false);
    // A Tasks session belongs to the pointer that opened it; the other is ignored.
    event(15, SV_POINT_TOOL4_HOLD, true);
    both(0, 0, 320, 0);
    sval_point_task();
    ++clock_ms;
    sval_point_task();
    assert(sval_point_output_session() && session_side == 1 && sval_point_blocked(0) && !sval_point_blocked(1));
    before = recognized;
    both(320, 0, 0, 0);
    assert(recognized == before && !pending_count(0));
    event(16, KC_ESC, true);
    event(16, KC_ESC, false);
    assert(!sval_point_output_session() && pointers[0].inhibited && pointers[1].inhibited);
    event(15, 4, false);
    assert(!sval_point_active(0) && !pointers[0].inhibited && !pointers[1].inhibited);
    puts("controller: one set of tool keys for both pointers, newest hold wins with no resume, toggle and Stop all, retired codes, status, task session owner passed");
    puts("controller: holds, remapped releases, masks, free diagonals, generation, session, suspend passed");
}
