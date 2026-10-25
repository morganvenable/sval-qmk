// The test runner extracts the actual combined hook and its state into this TU.
#include "sval.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pointing_motion.h"
#include "point_control.h"
#include "point_output.h"
#include "axis_scale.h"
#include "velocity_scroll.h"
#include "point_control.c"
static uint32_t clock_ms;
static uint16_t sensor_cpi = 800;
static uint16_t last_consumer;
static int      configured = USB_DEVICE_STATE_CONFIGURED;
static bool     connected;
uint32_t        timer_read32(void) {
           return clock_ms;
}
uint32_t timer_elapsed32(uint32_t t) {
    return clock_ms - t;
}
static uint8_t hand_sensor[2];
uint8_t        sval_pointer_sensor_for_hand(bool left) {
           return hand_sensor[left ? 0 : 1];
}
uint16_t timer_read(void) {
    return clock_ms;
}
uint16_t timer_elapsed(uint16_t t) {
    return (uint16_t)(clock_ms - t);
}
bool is_keyboard_master(void) {
    return true;
}
bool sval_other_half_connected(void) {
    return connected;
}
int usb_device_state_get_configure_state(void) {
    return configured;
}
usb_hid_protocol_t usb_device_state_get_protocol(void) {
    return USB_PROTOCOL_REPORT;
}
void sval_point_transport_install(void) {}
bool sval_point_transport_keyboard(report_keyboard_t *r) {
    (void)r;
    return true;
}
bool sval_point_transport_nkro(report_nkro_t *r) {
    (void)r;
    return true;
}
bool sval_point_transport_extra(report_extra_t *r) {
    if (r->report_id == REPORT_ID_CONSUMER) last_consumer = r->usage;
    return true;
}
static struct {
    bool     left_scroll, right_scroll, left_automouse, right_automouse, axis_scroll_lock, natural_scroll;
    uint8_t  automouse_decay;
    uint16_t automouse_threshold;
} global_saved_values;
typedef enum { OS_MACOS, OS_IOS, OS_WINDOWS } os_variant_t;
bool process_detected_host_os_user(os_variant_t os) {
    (void)os;
    return true;
}
uint16_t get_left_dpi(void) {
    return sensor_cpi;
}
uint16_t get_right_dpi(void) {
    return sensor_cpi;
}
void mouse_mode(bool enabled) {
    (void)enabled;
}
report_mouse_t pointing_device_task_user(report_mouse_t r) {
    return r;
}
report_mouse_t pointing_device_combine_reports(report_mouse_t a, report_mouse_t b) {
    a.x += b.x;
    a.y += b.y;
    a.h += b.h;
    a.v += b.v;
    a.buttons |= b.buttons;
    return a;
}
#ifdef SVAL_NATIVE_PTP
// The native touchpad's grab (azoteq/ptp.c), as the hook sees it.
#    include "ptp.h"
#    include "ptp_descriptor.h"
static bool     ptp_grabbed, ptp_ready[2];
static uint16_t ptp_touch[2], ptp_x[2], ptp_y[2];
static unsigned ptp_grab_calls;
uint16_t        sval_ptp_take_motion(uint8_t pad) {
           (void)pad;
           return 0;
}
void sval_ptp_set_grab(bool grab) {
    ptp_grabbed = grab;
    ++ptp_grab_calls;
}
bool sval_ptp_grab_ready(uint8_t pad) {
    return ptp_ready[pad & 1];
}
uint16_t sval_ptp_grab_finger(uint8_t pad, uint16_t *x, uint16_t *y) {
    if (!ptp_grabbed || !ptp_touch[pad & 1]) return 0;
    *x = ptp_x[pad & 1];
    *y = ptp_y[pad & 1];
    return ptp_touch[pad & 1];
}
uint8_t sval_pad_hands(void) {
    return (hand_sensor[0] == SVAL_SENSOR_AZOTEQ ? 1 : 0) | (hand_sensor[1] == SVAL_SENSOR_AZOTEQ ? 2 : 0);
}
#endif
#include "adapter.inc"
static report_mouse_t input(int lx, int ly, int rx, int ry, unsigned dt) {
    clock_ms += dt;
    report_mouse_t a = {.x = lx, .y = ly}, b = {.x = rx, .y = ry};
    return pointing_device_task_combined_user(a, b);
}
static void key(uint8_t pos, uint16_t code, bool down) {
    keyrecord_t r = {.event = {.key = {.row = pos / 6, .col = pos % 6}, .pressed = down}};
    assert(!sval_point_process(code, &r));
}
int main(void) {
    process_detected_host_os_kb(OS_WINDOWS);
    // Last ordinary wheel batch flushes with no new sensor motion.
    global_saved_values.left_scroll = true;
    report_mouse_t r                = input(12, -18, 0, 0, 1);
    assert(!r.x && !r.y && !r.h && !r.v);
    r = input(0, 0, 0, 0, 11);
    assert(r.h == 12 && r.v == 18);
    // Free diagonal control output consumes cursor and wheel exactly once.
    key(0, SV_POINT_TOOL1_HOLD, true);
    r = input(80, -64, 0, 0, 1);
    assert(!r.x && !r.y && !r.h && !r.v);
    r = input(0, 0, 0, 0, 11);
    assert(!r.h && !r.v);
    sval_point_task();
    assert(last_consumer == 0xE9);
    ++clock_ms;
    sval_point_task();
    assert(last_consumer == 0);
    key(0, SV_POINT_TOOL1_HOLD, false);
    // Natural scrolling changes only wheel sign.
    global_saved_values.natural_scroll = true;
    input(0, -20, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(r.v == -20);
    // Lock and control use one state; mode changes do not reset it.
    global_saved_values.axis_scroll_lock = true;
    input(0, -40, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_V && r.v == -40);
    key(1, SV_POINT_TOOL3_HOLD, true);
    input(20, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_V && !pending_count(0));
    input(160, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_H && !pending_count(0));
    // Breakaway batch was discarded, so a fresh Tabs step (320 counts) is required.
    input(320, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(pending_count(0));
    // Timeout is delivered with stopped sensors and invalidates pending steps.
    input(0, 0, 0, 0, 501);
    assert(scroll_policy.mode == SV_AXIS_FREE);
    // Host bypass is shared by control and wheel output.
    process_detected_host_os_kb(OS_MACOS);
    key(1, SV_POINT_TOOL3_HOLD, false);
    input(120, -120, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(r.h == 10 && r.v == -10);
    // Configure slot 1 as proportional scrolling through the actual protocol.
    sval_point_stop();
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    uint8_t packet[32] = {0xDF, sval_cmd_point_tools_get};
    sval_point_command(packet, 27);
    uint8_t lo = packet[4], hi = packet[5];
    memmove(packet + 5, packet + 6, 19);
    packet[1]  = sval_cmd_point_tools_set;
    packet[2]  = 5;
    packet[3]  = lo;
    packet[4]  = hi;
    packet[21] = (packet[21] & 0xE0) | SVAL_TOOL_PROPORTIONAL;
    sval_point_command(packet, 27);
    assert(!packet[2]);
    process_detected_host_os_kb(OS_WINDOWS);
    global_saved_values.natural_scroll   = false;
    global_saved_values.axis_scroll_lock = false;
    global_saved_values.left_scroll = global_saved_values.right_scroll = false;
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(71, -71, 0, 0, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0 && r.v > 0 && !r.x && !r.y);
    r = input(0, 0, 3, 4, 10);
    assert(r.h > 0 && r.v > 0 && !r.x && !r.y); // tools aren't per side: the other pointer runs it too
    key(2, SV_POINT_TOOL1_HOLD, false);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    key(2, SV_POINT_TOOL1_TOGGLE, true);
    key(2, SV_POINT_TOOL1_TOGGLE, false);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v); // new toggle starts centered
    input(71, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0 && !r.v);
    input(-71, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    key(2, SV_POINT_TOOL1_TOGGLE, true);
    key(2, SV_POINT_TOOL1_TOGGLE, false);
    // Keep the shared vertical lock across sensor-idle time, then break it using
    // ordinary policy motion. No second acquisition/breakaway implementation.
    scroll_policy                        = (sval_scroll_policy_t){0};
    global_saved_values.axis_scroll_lock = true;
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(20, -71, 0, 0, 1);
    r = input(0, 0, 0, 0, 5);
    assert(!r.h && !r.v); // no initial H leakage before V acquisition
    input(0, 0, 0, 0, 6);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && r.v > 0);
    key(2, SV_POINT_TOOL1_HOLD, false);
    scroll_policy = (sval_scroll_policy_t){0};
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(0, -71, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_V);
    for (int i = 0; i < 70; ++i) {
        r = input(0, 0, 0, 0, 10);
        assert(!r.h && r.v > 0 && scroll_policy.mode == SV_AXIS_LOCKED_V);
    }
    input(200, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_H && !r.v);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0 && !r.v);
    input(-200, 71, 0, 0, 1);
    r = input(0, 0, 0, 0, 11);
    assert(!r.h && !r.v); // full original center retained
    key(2, SV_POINT_TOOL1_HOLD, false);
    global_saved_values.axis_scroll_lock = false;
    global_saved_values.natural_scroll   = true;
    key(3, SV_POINT_TOOL1_HOLD, true);
    input(0, 0, 0, -71, 1); // only the right pointer moves
    r = input(0, 0, 0, 0, 10);
    assert(r.v < 0 && !r.h);
    key(4, SV_POINT_STOP_ALL, true);
    key(4, SV_POINT_STOP_ALL, false);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    key(3, SV_POINT_TOOL1_HOLD, false);
    global_saved_values.natural_scroll = false;
    // One held key drives both pointers when both move; each keeps its own center.
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(71, 0, 0, -71, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0 && r.v > 0 && !r.x && !r.y);
    // A second Proportional scroll key re-centers both; releasing the first keeps the tool.
    key(3, SV_POINT_TOOL1_HOLD, true);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    input(71, 0, 0, -71, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0 && r.v > 0);
    key(2, SV_POINT_TOOL1_HOLD, false);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0 && r.v > 0);
    key(3, SV_POINT_TOOL1_HOLD, false);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    // Retired codes (the right pointer's keys in early test builds) do nothing.
    for (uint16_t code = SV_POINT_FIRST + 8; code < SV_POINT_STOP_ALL; ++code) {
        key(3, code, true);
        r = input(71, 0, 0, 0, 1);
        assert(r.x == 71 && !sval_point_active(0) && !sval_point_active(1));
        input(0, 0, 0, 0, 10);
        key(3, code, false);
    }
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(71, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0);
    key(3, SV_POINT_TOOL2_HOLD, true);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    key(3, SV_POINT_TOOL2_HOLD, false);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v); // restored tool starts centered
    key(2, SV_POINT_TOOL1_HOLD, false);
    key(2, SV_POINT_TOOL1_TOGGLE, true);
    key(2, SV_POINT_TOOL1_TOGGLE, false);
    input(71, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0);
    configured = USB_DEVICE_STATE_SUSPEND;
    sval_point_task();
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v && !sval_point_active(0));
    configured = USB_DEVICE_STATE_CONFIGURED;
    sval_point_task();
    connected = true;
    sval_point_task();
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(71, 0, 0, 0, 1);
    r = input(0, 0, 0, 0, 10);
    assert(r.h > 0);
    connected = false;
    sval_point_task();
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v && !sval_point_active(0));
    key(2, SV_POINT_TOOL1_HOLD, false);
    key(2, SV_POINT_TOOL1_HOLD, true);
    input(71, 0, 0, 0, 1);
    input(0, 0, 0, 0, 10);
    key(3, SV_POINT_TOOL4_HOLD, true); // newest wins on both pointers: Tasks
    input(0, 0, 320, 0, 1);
    sval_point_task();
    input(0, 0, 0, 0, 11);
    sval_point_task();
    assert(sval_point_output_session());
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v);
    key(3, SV_POINT_TOOL4_HOLD, false);
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v); // no prior velocity resumes after task selection
    key(2, SV_POINT_TOOL1_HOLD, false);
    // Tools normalize a trackball by its CPI, but take a TrackPoint's counts as they come.
    sval_point_stop();
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    global_saved_values.axis_scroll_lock = true;
    scroll_policy                        = (sval_scroll_policy_t){0};
    sensor_cpi                           = 1600;
    hand_sensor[0]                       = SVAL_SENSOR_PMW3389;
    clock_ms += 600;
    key(5, SV_POINT_TOOL2_HOLD, true);
    input(170, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(!pending_count(0));
    key(5, SV_POINT_TOOL2_HOLD, false);
    hand_sensor[0] = SVAL_SENSOR_TRACKPOINT;
    input(0, 0, 0, 0, 600); // the module is looked up every 500 ms
    key(5, SV_POINT_TOOL2_HOLD, true);
    // A TrackPoint ignores the 1600 CPI: logical 800 CPI times its 2.0 gain, so 80 counts
    // make the 160-count Arrows step.
    input(79, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(!pending_count(0));
    input(1, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(pending_count(0));
    key(5, SV_POINT_TOOL2_HOLD, false);
    sval_point_stop();
    sensor_cpi     = 800;
    hand_sensor[0] = SVAL_SENSOR_NONE;
    // Sniper and boost never change a tool's feel: same travel per step, same
    // Proportional scroll speed. A pointer without a tool still gets both.
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    global_saved_values.axis_scroll_lock = false;
    global_saved_values.left_scroll = global_saved_values.right_scroll = false;
    scroll_policy                                                      = (sval_scroll_policy_t){0};
    clock_ms += 600;
    key(5, SV_POINT_TOOL2_HOLD, true); // slot 2: Arrows, 160 counts a step
    uint32_t steps = recognized;
    input(170, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1);
    sniper_hold_5 = 1;
    update_sniper_divisor();
    steps = recognized;
    input(170, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1); // not 170 / 5 = 34 counts
    steps = recognized;
    r     = input(0, 0, 170, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1 && !r.x); // the other pointer runs the tool, unslowed too
    sniper_hold_5 = 0;
    update_sniper_divisor();
    boost_hold_5 = 1;
    update_boost_multiplier();
    steps = recognized;
    input(170, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1); // not 5 steps
    steps = recognized;
    input(0, 0, 170, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1);
    key(5, SV_POINT_TOOL2_HOLD, false);
    // With no tool on, both pointers get sniper and boost as usual.
    r = input(0, 0, 10, 0, 1);
    assert(r.x == 50);
    boost_hold_5 = 0;
    update_boost_multiplier();
    sniper_hold_5 = 1;
    update_sniper_divisor();
    r = input(50, 0, 50, 0, 1);
    assert(r.x == 20);
    sniper_hold_5 = 0;
    update_sniper_divisor();
    // Slot 1 is Proportional scroll (set above): the speed for the same displacement.
    int16_t speed[3];
    for (int run = 0; run < 3; ++run) {
        sniper_hold_5 = run == 1;
        boost_hold_5  = run == 2;
        update_sniper_divisor();
        update_boost_multiplier();
        key(6, SV_POINT_TOOL1_HOLD, true);
        input(71, 0, 0, 0, 1);
        r = input(0, 0, 0, 0, 10);
        input(0, 0, 0, 0, 10);
        r          = input(0, 0, 0, 0, 10);
        speed[run] = r.h;
        key(6, SV_POINT_TOOL1_HOLD, false);
        input(0, 0, 0, 0, 10);
    }
    assert(speed[0] > 0 && speed[1] == speed[0] && speed[2] == speed[0]);
    // The motion a tool pointer feeds the shared axis lock isn't slowed either.
    sniper_hold_5 = 1;
    boost_hold_5  = 0;
    update_sniper_divisor();
    update_boost_multiplier();
    global_saved_values.axis_scroll_lock = true;
    scroll_policy                        = (sval_scroll_policy_t){0};
    key(7, SV_POINT_TOOL3_HOLD, true); // slot 3: Tabs
    input(0, -40, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_V);
    input(160, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(scroll_policy.mode == SV_AXIS_LOCKED_H); // the same breakaway as without sniper
    key(7, SV_POINT_TOOL3_HOLD, false);
    global_saved_values.axis_scroll_lock = false;
    sniper_hold_5 = boost_hold_5 = 0;
    update_sniper_divisor();
    update_boost_multiplier();
    // Velocity Scroll owns every pointer while held: a tool key held with it sees no motion.
    key(8, SV_POINT_TOOL1_HOLD, true);
    vs_keys = 1;
    velocity_scroll_update();
    r = input(71, 0, 0, 0, 1);
    assert(!r.x && !r.y);
    vs_keys = 0;
    velocity_scroll_update();
    input(0, 0, 0, 0, 10);
    r = input(0, 0, 0, 0, 10);
    assert(!r.h && !r.v); // slot 1 is Proportional: it never saw the 71 counts
    key(8, SV_POINT_TOOL1_HOLD, false);
    input(0, 0, 0, 0, 10);
    puts("combined adapter: Velocity Scroll owns every pointer while held, tools included passed");
    puts("combined adapter: sniper and boost leave tool steps and Proportional scroll alone, still scale the other pointer passed");
    // Mixed modules: a touchpad on the left, a trackball on the right, one tool on both.
    // The pad's counts take its 2.0 gain (80 counts a 160-count Arrows step at 800 CPI),
    // the ball's only its CPI (160). The pad's own clicks and wheel are dropped while the
    // tool runs; the ball's report keeps its buttons.
    sval_point_stop();
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
    sensor_cpi                           = 800;
    hand_sensor[0]                       = SVAL_SENSOR_AZOTEQ;
    hand_sensor[1]                       = SVAL_SENSOR_PMW3389;
    global_saved_values.axis_scroll_lock = false;
    global_saved_values.left_scroll = global_saved_values.right_scroll = false;
    input(0, 0, 0, 0, 600);
    key(9, SV_POINT_TOOL2_HOLD, true);
    steps = recognized;
    input(79, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps);
    input(1, 0, 0, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1);
    steps = recognized;
    input(0, 0, 159, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps);
    input(0, 0, 1, 0, 1);
    input(0, 0, 0, 0, 11);
    assert(recognized == steps + 1);
    {
        clock_ms += 1;
        report_mouse_t pad = {.buttons = 1, .v = 3, .h = 2}, ball = {.buttons = 2};
        r = pointing_device_task_combined_user(pad, ball);
        assert(r.buttons == 2 && !r.h && !r.v);
    }
    uint8_t status[32] = {0xDF, sval_cmd_point_tools_status};
    sval_point_command(status, 27);
    assert(status[2] == 2 && status[21] == SVAL_POINT_DEVICE_PAD && status[22] == SVAL_POINT_DEVICE_BALL && status[23] == 0);
    key(9, SV_POINT_TOOL2_HOLD, false);
    {
        // With no tool on, the pad's click goes through again.
        clock_ms += 1;
        report_mouse_t pad = {.buttons = 1}, ball = {0};
        r = pointing_device_task_combined_user(pad, ball);
        assert(r.buttons == 1);
    }
    hand_sensor[0] = hand_sensor[1] = SVAL_SENSOR_NONE;
    input(0, 0, 0, 0, 600);
    puts("combined adapter: touchpad left and trackball right run one tool, each at its own device scale; pad clicks and wheel dropped while it runs passed");
    puts("combined adapter: TrackPoint counts at logical 800 CPI times its gain, trackball counts normalized by CPI passed");

    // TrackPoint Proportional scroll (slot 1): the push itself, its counts over the last
    // 64 ms, is the offset. A steady push scrolls at a steady speed, and letting go stops
    // it within the window. A trackball given the same counts keeps speeding up and keeps
    // scrolling after it stops, as an offset from the origin should.
    {
        int32_t speed_at[2][3];
        for (int run = 0; run < 2; ++run) {
            hand_sensor[0] = run ? SVAL_SENSOR_PMW3389 : SVAL_SENSOR_TRACKPOINT;
            input(0, 0, 0, 0, 600);
            key(10, SV_POINT_TOOL1_HOLD, true);
            // 10 counts every 10 ms for 500 ms; the scroll over 100-250 ms and 350-500 ms.
            speed_at[run][0] = speed_at[run][1] = 0;
            for (int t = 1; t <= 50; ++t) {
                r = input(10, 0, 0, 0, 10);
                if (t > 10 && t <= 25) speed_at[run][0] += r.h;
                if (t > 35) speed_at[run][1] += r.h;
            }
            for (int t = 0; t < 10; ++t)
                input(0, 0, 0, 0, 10); // let go: 100 ms
            speed_at[run][2] = 0;
            for (int t = 0; t < 10; ++t)
                speed_at[run][2] += input(0, 0, 0, 0, 10).h;
            key(10, SV_POINT_TOOL1_HOLD, false);
            input(0, 0, 0, 0, 10);
        }
        // Stick: the same speed early and late, nothing after the push.
        assert(speed_at[0][0] > 0 && abs(speed_at[0][1] - speed_at[0][0]) * 5 <= speed_at[0][0] && speed_at[0][2] == 0);
        // Ball: faster late than early, and still scrolling after it stops.
        assert(speed_at[1][1] > 2 * speed_at[1][0] && speed_at[1][2] > 0);
        hand_sensor[0] = SVAL_SENSOR_NONE;
        input(0, 0, 0, 0, 600);
    }
    puts("combined adapter: TrackPoint Proportional scroll follows the push (steady speed, stops on release); trackball keeps its origin passed");
#ifdef SVAL_NATIVE_PTP
    // Native touchpad: while a tool runs, the firmware has the pad's contacts and the
    // operating system's touchpad gets nothing (the grab); the pad's own mouse output is
    // ignored. Counts: 2048 across 43 mm, so at 800 CPI 121 pad counts are 80 counts,
    // one 160-count Arrows step at the pad's 2.0 gain.
    {
        hand_sensor[0] = SVAL_SENSOR_AZOTEQ;
        hand_sensor[1] = SVAL_SENSOR_PMW3389;
        ptp_ready[0]   = true;
        input(0, 0, 0, 0, 600);
        assert(!ptp_grabbed);
        key(11, SV_POINT_TOOL2_HOLD, true);
        input(0, 0, 0, 0, 1);
        assert(ptp_grabbed);
        uint32_t steps = recognized;
        // A finger lands: never motion, and the pad's own relative output is ignored.
        ptp_touch[0] = 7;
        ptp_x[0]     = 1000;
        ptp_y[0]     = 900;
        input(500, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        assert(recognized == steps);
        ptp_x[0] += 120; // 79.7 counts: not yet
        input(0, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        assert(recognized == steps);
        ptp_x[0] += 2;
        input(0, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        assert(recognized == steps + 1);
        uint8_t status[32] = {0xDF, sval_cmd_point_tools_status};
        sval_point_command(status, 27);
        assert(status[21] == SVAL_POINT_DEVICE_PAD && status[22] == SVAL_POINT_DEVICE_BALL && status[23] == 1);
        // Lifting to reposition: part of a step, a lift longer than the 250 ms idle, a
        // landing far to the left, the rest of the step. One step, to the right: the lift
        // neither ended the step nor counted the jump back as a reversal.
        steps = recognized;
        ptp_x[0] += 100; // 66 counts, 132 of 160
        input(0, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        ptp_touch[0] = 0;
        for (int t = 0; t < 40; ++t)
            input(0, 0, 0, 0, 10);
        ptp_touch[0] = 8;
        ptp_x[0]     = 200;
        input(0, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        assert(recognized == steps);
        ptp_x[0] += 50; // 33 counts more: 198 of 160
        input(0, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        assert(recognized == steps + 1 && pointers[0].pending[0].direction > 0);
        key(11, SV_POINT_TOOL2_HOLD, false);
        input(0, 0, 0, 0, 1);
        assert(!ptp_grabbed);
        // Proportional scroll (slot 1) on the pad: anchored where the finger lands, stops
        // when it lifts, anchored again where it lands next.
        key(11, SV_POINT_TOOL1_HOLD, true);
        input(0, 0, 0, 0, 1);
        assert(ptp_grabbed);
        ptp_touch[0] = 9;
        ptp_x[0]     = 300;
        input(0, 0, 0, 0, 10);
        ptp_x[0] += 300;
        input(0, 0, 0, 0, 10);
        int32_t far = 0;
        for (int t = 0; t < 10; ++t)
            far += input(0, 0, 0, 0, 10).h;
        assert(far > 0);
        ptp_touch[0] = 0;
        input(0, 0, 0, 0, 10);
        int32_t up = 0;
        for (int t = 0; t < 10; ++t)
            up += input(0, 0, 0, 0, 10).h;
        assert(up == 0);
        ptp_touch[0]   = 10;
        ptp_x[0]       = 1800; // lands far to the right: no offset yet
        int32_t landed = 0;
        for (int t = 0; t < 10; ++t)
            landed += input(0, 0, 0, 0, 10).h;
        assert(landed == 0);
        ptp_x[0] -= 100; // a small move left of the new anchor
        input(0, 0, 0, 0, 10);
        int32_t back = 0;
        for (int t = 0; t < 10; ++t)
            back += input(0, 0, 0, 0, 10).h;
        assert(back < 0 && -back < far);
        key(11, SV_POINT_TOOL1_HOLD, false);
        input(0, 0, 0, 0, 1);
        assert(!ptp_grabbed);
        // Velocity Scroll and a tool share the one grab: it stays while either wants it.
        key(11, SV_POINT_TOOL2_HOLD, true);
        input(0, 0, 0, 0, 1);
        vs_keys = 1;
        velocity_scroll_update();
        assert(ptp_grabbed);
        key(11, SV_POINT_TOOL2_HOLD, false);
        input(0, 0, 0, 0, 1);
        assert(ptp_grabbed);
        vs_keys = 0;
        velocity_scroll_update();
        assert(!ptp_grabbed);
        // A pad whose contacts don't reach the firmware (not ready) runs on its own mouse
        // output, as in a build without the native touchpad.
        ptp_touch[0] = 0;
        ptp_ready[0] = false;
        key(11, SV_POINT_TOOL2_HOLD, true);
        steps = recognized;
        input(80, 0, 0, 0, 1);
        input(0, 0, 0, 0, 11);
        assert(recognized == steps + 1);
        key(11, SV_POINT_TOOL2_HOLD, false);
        input(0, 0, 0, 0, 1);
        hand_sensor[0] = hand_sensor[1] = SVAL_SENSOR_NONE;
        input(0, 0, 0, 0, 600);
    }
    puts("combined adapter (native touchpad): a tool takes the pads (grab) and reads the finger; landings are not motion; lifts keep a step going and re-anchor Proportional scroll; one grab with Velocity Scroll passed");
#endif
    puts("combined adapter: proportional hold/toggle, center, independent pointers, natural direction, shared lock sustain/breakaway and stop passed");
    puts("combined adapter: idle flush, no cursor/wheel leakage, natural scroll, shared lock/breakaway, idle timeout, host bypass passed");
}
