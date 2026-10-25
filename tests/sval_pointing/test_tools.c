#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include "point_control.c"
static uint32_t          clock_ms;
static report_keyboard_t last_down;
static unsigned          presses;
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
    return USB_DEVICE_STATE_CONFIGURED;
}
usb_hid_protocol_t usb_device_state_get_protocol(void) {
    return USB_PROTOCOL_REPORT;
}
void sval_point_transport_install(void) {}
bool sval_point_transport_keyboard(report_keyboard_t *r) {
    if (r->keys[0]) {
        last_down = *r;
        presses++;
    }
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
static void event(uint16_t code, bool down) {
    keyrecord_t r = {.event = {.key = {.row = 0, .col = 0}, .pressed = down}};
    assert(!sval_point_process(code, &r));
}
static void tick(void) {
    for (int i = 0; i < 8; ++i) {
        ++clock_ms;
        sval_point_task();
    }
}
static void configure(uint8_t action, uint8_t reverse) {
    sval_point_stop();
    tick();
    uint8_t p[32] = {0xDF, sval_cmd_point_tools_get};
    sval_point_command(p, 27);
    uint8_t lo = p[4], hi = p[5];
    memmove(p + 5, p + 6, 19);
    p[1]  = sval_cmd_point_tools_set;
    p[2]  = 5;
    p[3]  = lo;
    p[4]  = hi;
    p[21] = (p[21] & 0xE0) | action | (reverse << 3);
    sval_point_command(p, 27);
    assert(!p[2]);
    sval_point_config_reload();
    assert(sval_point_tool(1) == action);
    event(SV_POINT_TOOL1_HOLD, true);
}
// What QMK itself sends: the user's held modifiers and keys.
static void hold_base(uint8_t mods, uint8_t key) {
    report_keyboard_t base = {.mods = mods, .keys = {key}};
    sval_point_output_observe_keyboard(&base);
    tick();
}
// Motions come 200 ms apart: past every action's default pace (at most 150 ms) and inside
// the 250 ms that starts a new stroke.
static void motion(int x, int y, uint8_t axes) {
    clock_ms += 200;
    sval_point_buffer(0, x, y, 800);
    sval_point_finish_interval(axes);
    tick();
}
// Each action's own travel per step at the default speed (Tabs and Tasks 320 counts).
#define STEP(tool) ((int)sval_point_step_counts(tool, 64))
#define SWITCH STEP(SVAL_TOOL_TABS)
static void wait_rate(void) {
    clock_ms += sval_point_action_default(SVAL_TOOL_TABS)->interval_ms;
}
int main(void) {
    configure(SVAL_TOOL_ZOOM, 0);
    motion(0, -STEP(SVAL_TOOL_ZOOM), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_EQUAL && last_down.mods == 1);
    motion(0, STEP(SVAL_TOOL_ZOOM), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_MINUS && last_down.mods == 1);
    unsigned before = presses;
    motion(640, 0, SVAL_AXIS_BOTH);
    assert(presses == before);
    motion(0, -640, SVAL_AXIS_H);
    assert(presses == before); // shared axis policy still wins
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_ZOOM, 2);
    motion(0, -STEP(SVAL_TOOL_ZOOM), SVAL_AXIS_V);
    assert(last_down.keys[0] == KC_MINUS);
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_HISTORY, 0);
    motion(-STEP(SVAL_TOOL_HISTORY), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && last_down.mods == 1);
    motion(STEP(SVAL_TOOL_HISTORY), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && last_down.mods == 3);
    before = presses;
    motion(0, -640, SVAL_AXIS_BOTH);
    assert(presses == before);
    // A stopped ball does not replay surplus undo/redo actions.
    sval_point_buffer(0, 0, 0, 800);
    tick();
    assert(presses == before);
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_HISTORY_Y, 1);
    motion(-STEP(SVAL_TOOL_HISTORY_Y), 0, SVAL_AXIS_H);
    assert(last_down.keys[0] == KC_Y && last_down.mods == 1);
    event(SV_POINT_TOOL1_HOLD, false);
    // Session lifecycle follows the selected tool, not the original slot label.
    configure(SVAL_TOOL_TASKS, 0);
    motion(SWITCH - 1, 0, SVAL_AXIS_H);
    assert(!sval_point_output_session()); // a Tasks step is 320 counts, not 64
    motion(1, 0, SVAL_AXIS_H);
    assert(sval_point_output_session());
    event(SV_POINT_TOOL1_HOLD, false);
    tick();
    assert(!sval_point_output_session());
    // Arrows carry the held modifiers: Shift+arrow selects, Ctrl+arrow jumps words.
    configure(SVAL_TOOL_ARROWS, 0);
    hold_base(MOD_BIT(KC_LSFT), 0);
    motion(STEP(SVAL_TOOL_ARROWS), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_RIGHT && last_down.mods == MOD_BIT(KC_LSFT));
    hold_base(MOD_BIT(KC_LCTL) | MOD_BIT(KC_LSFT), 0);
    motion(-STEP(SVAL_TOOL_ARROWS), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_LEFT && last_down.mods == (MOD_BIT(KC_LCTL) | MOD_BIT(KC_LSFT)));
    hold_base(MOD_BIT(KC_LCTL), 0);
    motion(0, -STEP(SVAL_TOOL_ARROWS), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_UP && last_down.mods == MOD_BIT(KC_LCTL));
    // A held key (not a modifier) still holds Arrows back.
    hold_base(0, KC_Z);
    motion(0, -STEP(SVAL_TOOL_ARROWS), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && !last_down.keys[1]); // only the user's own key
    hold_base(0, 0);
    motion(0, -STEP(SVAL_TOOL_ARROWS), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_UP && !last_down.mods);
    event(SV_POINT_TOOL1_HOLD, false);
    // Tabs, Tasks, Zoom and both Undo / Redo still wait for held modifiers: an extra
    // modifier would change what the shortcut means.
    const uint8_t blocked[] = {SVAL_TOOL_TABS, SVAL_TOOL_TASKS, SVAL_TOOL_ZOOM, SVAL_TOOL_HISTORY, SVAL_TOOL_HISTORY_Y};
    for (unsigned i = 0; i < sizeof(blocked); ++i) {
        configure(blocked[i], 0);
        const int step = STEP(blocked[i]);
        const int dx = blocked[i] == SVAL_TOOL_ZOOM ? 0 : step, dy = blocked[i] == SVAL_TOOL_ZOOM ? -step : 0;
        hold_base(MOD_BIT(KC_LSFT), 0);
        before = presses;
        motion(dx, dy, SVAL_AXIS_BOTH);
        assert(presses == before && !sval_point_output_session());
        before = presses;
        hold_base(0, 0);
        motion(dx, dy, SVAL_AXIS_BOTH);
        assert(presses > before); // goes out once the modifier is up
        event(SV_POINT_TOOL1_HOLD, false);
        tick();
        assert(!sval_point_output_session());
    }
    // macOS: the Command shortcuts, with no setting.
    sval_point_host_mac(true);
    const uint8_t cmd = MOD_BIT(KC_LGUI), shift = MOD_BIT(KC_LSFT);
    configure(SVAL_TOOL_ZOOM, 0);
    motion(0, -STEP(SVAL_TOOL_ZOOM), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_EQUAL && last_down.mods == cmd);
    motion(0, STEP(SVAL_TOOL_ZOOM), SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_MINUS && last_down.mods == cmd);
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_HISTORY, 0);
    motion(-STEP(SVAL_TOOL_HISTORY), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && last_down.mods == cmd);
    motion(STEP(SVAL_TOOL_HISTORY), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && last_down.mods == (cmd | shift));
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_HISTORY_Y, 0);
    motion(STEP(SVAL_TOOL_HISTORY_Y), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && last_down.mods == (cmd | shift)); // Mac redo, not Command+Y
    motion(-STEP(SVAL_TOOL_HISTORY_Y), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_Z && last_down.mods == cmd);
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_TABS, 0);
    motion(SWITCH, 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_RBRC && last_down.mods == (cmd | shift));
    wait_rate();
    motion(-SWITCH, 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_LBRC && last_down.mods == (cmd | shift));
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_TASKS, 0);
    motion(SWITCH, 0, SVAL_AXIS_BOTH);
    assert(sval_point_output_session() && last_down.keys[0] == KC_TAB && last_down.mods == cmd);
    wait_rate();
    motion(-SWITCH, 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_TAB && last_down.mods == (cmd | shift));
    event(SV_POINT_TOOL1_HOLD, false);
    tick();
    assert(!sval_point_output_session());
    configure(SVAL_TOOL_ARROWS, 0);
    motion(STEP(SVAL_TOOL_ARROWS), 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_RIGHT && !last_down.mods); // arrows are the same keys
    event(SV_POINT_TOOL1_HOLD, false);
    // Back on Windows: the Ctrl and Alt shortcuts again.
    sval_point_host_mac(false);
    configure(SVAL_TOOL_TABS, 0);
    motion(SWITCH, 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_TAB && last_down.mods == MOD_BIT(KC_LCTL));
    event(SV_POINT_TOOL1_HOLD, false);
    configure(SVAL_TOOL_TASKS, 0);
    motion(SWITCH, 0, SVAL_AXIS_BOTH);
    assert(last_down.keys[0] == KC_TAB && last_down.mods == MOD_BIT(KC_LALT));
    event(SV_POINT_TOOL1_HOLD, false);
    tick();
    puts("tools: arrows carry held modifiers; tabs, tasks, zoom and undo / redo wait for them; macOS Command shortcuts passed");
    puts("tools: zoom, both redo shortcuts, direction, axes, no replay, persistence, remapped task release passed");
}
