#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include "point_control.c"
static uint32_t clock_ms;
static unsigned volume_up, volume_down, right, up, tab;
uint32_t        timer_read32(void) {
    return clock_ms;
}
bool is_keyboard_master(void) {
    return true;
}
bool sval_other_half_connected(void) {
    return false;
}
int usb_device_state_get_configure_state(void) {
    return USB_DEVICE_STATE_CONFIGURED;
}
usb_hid_protocol_t usb_device_state_get_protocol(void) {
    return USB_PROTOCOL_REPORT;
}
void sval_point_transport_install(void) {}
bool sval_point_transport_keyboard(report_keyboard_t *r) {
    for (unsigned i = 0; i < sizeof(r->keys); ++i) {
        right += r->keys[i] == KC_RIGHT;
        up += r->keys[i] == KC_UP;
        tab += r->keys[i] == KC_TAB;
    }
    return true;
}
bool sval_point_transport_nkro(report_nkro_t *r) {
    (void)r;
    return true;
}
bool sval_point_transport_extra(report_extra_t *r) {
    volume_up += r->usage == 0xE9;
    volume_down += r->usage == 0xEA;
    return true;
}
static void tick(unsigned n) {
    while (n--) {
        ++clock_ms;
        sval_point_task();
    }
}
static void activate(uint16_t key) {
    sval_point_stop();
    tick(100);
    keyrecord_t r = {.event = {.key = {.row = 0, .col = 0}, .pressed = true}};
    sval_point_process(key, &r);
    r.event.pressed = false;
    sval_point_process(key, &r);
}
static void motion(int x, int y) {
    sval_point_buffer(0, x, y, 800);
    sval_point_finish_interval(SVAL_AXIS_BOTH);
    sval_point_task();
}
static void stopped(void) {
    sval_point_buffer(0, 0, 0, 800);
}
static void bits(uint8_t *p, int start, int n, int value) {
    for (int i = 0; i < n; ++i) {
        int b    = start + i;
        p[b / 8] = (p[b / 8] & ~(1 << (b % 8))) | (((value >> i) & 1) << (b % 8));
    }
}
int main(void) {
    activate(SV_POINT_TOOL1_TOGGLE);
    motion(0, -20 * 64);
    assert(pending_count(0) >= 19); // No arbitrary eight-step burst cap.
    tick(8);
    unsigned before = volume_up;
    assert(before > 1);
    stopped();
    tick(500);
    assert(volume_up == before); // No catch-up after zero-motion report.
    activate(SV_POINT_TOOL1_TOGGLE);
    before = volume_up;
    for (int i = 0; i < 100; ++i) {
        motion(0, -64);
        tick(5);
    }
    assert(volume_up - before > 25); // Uncapped removes the old 25/sec ceiling.
    stopped();
    before = volume_up;
    tick(500);
    assert(volume_up == before);
    activate(SV_POINT_TOOL2_TOGGLE);
    motion(2 * 160, -2 * 160); // Arrows: 160 counts a step, about 15 a second
    tick(300);
    assert(right == 2 && up == 2);
    activate(SV_POINT_TOOL1_TOGGLE);
    before = volume_up;
    motion(0, -20 * 64);
    tick(2);
    unsigned now = volume_up;
    motion(0, 64);
    tick(8);
    assert(volume_up == now && volume_down == 1);
    stopped();
    // A visible 10/sec rate cap does not create delayed motion debt.
    sval_point_stop();
    tick(100);
    uint8_t p[32] = {0xDF, sval_cmd_point_tools_get};
    sval_point_command(p, 27);
    uint8_t lo = p[4], hi = p[5];
    memmove(p + 5, p + 6, 19);
    p[1] = sval_cmd_point_tools_set;
    p[2] = 5;
    p[3] = lo;
    p[4] = hi;
    bits(p + 5, 28, 8, 100);
    sval_point_command(p, 27);
    assert(p[2] == 0);
    activate(SV_POINT_TOOL1_TOGGLE);
    before = volume_up;
    for (int i = 0; i < 100; ++i) {
        motion(0, -10 * 64);
        tick(5);
    }
    assert(volume_up - before == 6);
    stopped();
    before = volume_up;
    tick(1000);
    assert(volume_up == before);
    activate(SV_POINT_TOOL1_TOGGLE);
    motion(0, -20 * 64);
    sval_point_stop();
    before = volume_up;
    tick(500);
    assert(volume_up == before);
    puts("direct motion: no burst cap, no stopped-motion replay, uncapped throughput, explicit rate cap, independent axes and reversal passed");
}
