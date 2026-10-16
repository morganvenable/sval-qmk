// Hard flicks through the real frame pipeline: a sensor report every 1 ms, a scroll batch
// every 10 ms (keymap_support.c's SCROLL_FREQUENCY_MS), the tools task every 1 ms. For every
// discrete tool: presses never come faster than the rate limit, and once the ball stops no
// press goes out after the batch it stopped in. Motion over the limit is dropped, not queued.
#include "sval.h"
#include <assert.h>
#include <stdio.h>
#include "point_control.c"
#define BATCH_MS 10
static uint32_t clock_ms;
static bool     key_down, consumer_down;
static uint32_t press_time[4096];
static unsigned presses;
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
void        sval_point_transport_install(void) {}
static void record(bool down, bool *was) {
    if (down && !*was) {
        assert(presses < sizeof(press_time) / sizeof(press_time[0]));
        press_time[presses++] = clock_ms;
    }
    *was = down;
}
bool sval_point_transport_keyboard(report_keyboard_t *r) {
    // Escape is the Tasks session's own cancel (its 10 s timeout), not a step.
    bool down = false;
    for (unsigned i = 0; i < sizeof(r->keys); ++i)
        down |= r->keys[i] && r->keys[i] != KC_ESC;
    record(down, &key_down);
    return true;
}
bool sval_point_transport_nkro(report_nkro_t *r) {
    (void)r;
    return true;
}
bool sval_point_transport_extra(report_extra_t *r) {
    record(r->usage != 0, &consumer_down);
    return true;
}
static void bits(uint8_t *p, int start, int n, int value) {
    for (int i = 0; i < n; ++i) {
        int b    = start + i;
        p[b / 8] = (p[b / 8] & ~(1 << (b % 8))) | (((value >> i) & 1) << (b % 8));
    }
}
static void settle(void) {
    for (int i = 0; i < 20; ++i) {
        ++clock_ms;
        sval_point_task();
    }
}
// Slot 1 runs the tool, with a rate limit in ms (0: none set).
static void configure(uint8_t tool, uint8_t interval) {
    sval_point_stop();
    settle();
    uint8_t p[32] = {0xDF, sval_cmd_point_tools_get};
    sval_point_command(p, 26);
    uint8_t lo = p[4], hi = p[5];
    memmove(p + 5, p + 6, 19);
    p[1] = sval_cmd_point_tools_set;
    p[2] = 5;
    p[3] = lo;
    p[4] = hi;
    bits(p + 5, 28, 8, interval);
    p[21] = (p[21] & 0xE0) | tool;
    sval_point_command(p, 26);
    assert(p[2] == 0 && sval_point_tool(1) == tool);
}
static void frame(int x, int y) {
    ++clock_ms;
    sval_point_buffer(0, x, y, 800);
    if (clock_ms % BATCH_MS == 0) sval_point_finish_interval(SVAL_AXIS_BOTH);
    sval_point_task();
}
static bool steady; // constant speed instead of falling to rest
static bool vertical(uint8_t tool) {
    return tool == SVAL_TOOL_VOLUME || tool == SVAL_TOOL_ZOOM;
}
// Flick for `ms` with motion falling from `peak` counts per ms to nothing, then stop for 3 s.
// Returns the presses it sent; asserts the pacing and that nothing follows the stop.
static unsigned flick(uint8_t tool, uint16_t interval, int peak, unsigned ms) {
    keyrecord_t r = {.event = {.key = {.row = 0, .col = 0}, .pressed = true}};
    sval_point_process(SV_POINT_TOOL1_HOLD, &r);
    unsigned first = presses;
    for (unsigned t = 0; t < ms; ++t) {
        int v = steady ? peak : peak - (int)((long)peak * t / ms);
        frame(vertical(tool) ? 0 : v, vertical(tool) ? -v : 0);
    }
    uint32_t stopped = clock_ms;
    for (unsigned t = 0; t < 3000; ++t)
        frame(0, 0);
    unsigned during = presses - first;
    // Steps stop within the batch the ball stopped in: none later than its end.
    for (unsigned i = first; i < presses; ++i)
        assert(press_time[i] <= stopped + BATCH_MS + 1);
    // Never faster than the rate limit.
    for (unsigned i = first + 1; i < presses; ++i)
        assert(!interval || press_time[i] - press_time[i - 1] >= interval);
    if (interval) assert(presses - first <= ms / interval + 2);
    r.event.pressed = false;
    sval_point_process(SV_POINT_TOOL1_HOLD, &r);
    settle();
    return during;
}
int main(void) {
    const uint8_t tools[] = {SVAL_TOOL_VOLUME, SVAL_TOOL_ARROWS, SVAL_TOOL_TABS, SVAL_TOOL_TASKS, SVAL_TOOL_ZOOM, SVAL_TOOL_HISTORY, SVAL_TOOL_HISTORY_Y};
    for (unsigned i = 0; i < sizeof(tools); ++i) {
        uint8_t tool = tools[i];
        // The tool's own pacing: Tabs and Tasks default to 140 ms, the others are uncapped.
        configure(tool, 0);
        uint16_t pace = sval_point_interval(1);
        assert(pace == (tool == SVAL_TOOL_TABS || tool == SVAL_TOOL_TASKS ? SVAL_POINT_SWITCH_INTERVAL_MS : 0));
        // A hard flick (about 3 m/s at 800 CPI, falling to rest over 400 ms), then stop.
        unsigned during = flick(tool, pace, 100, 400);
        assert(during >= 1);
        if (pace) assert(during <= 400u / pace + 1);
        // An explicit rate limit wins over the default, for every tool.
        configure(tool, 50);
        assert(sval_point_interval(1) == 50);
        during = flick(tool, 50, 100, 400);
        assert(during >= 2 && during <= 400 / 50 + 1);
        // A short hard spin: hundreds of steps of motion in 60 ms give a handful of presses.
        configure(tool, 0);
        flick(tool, pace, 200, 60);
    }
    // Tabs: one step per 320 counts (about 10 mm at 800 CPI), five times Volume's.
    configure(SVAL_TOOL_TABS, 0);
    assert(sval_point_step_scale(SVAL_TOOL_TABS) == 5 && sval_point_step_scale(SVAL_TOOL_VOLUME) == 1);
    steady          = true;
    unsigned during = flick(SVAL_TOOL_TABS, SVAL_POINT_SWITCH_INTERVAL_MS, 1, 319);
    assert(during == 0); // 319 counts: not a step yet
    during = flick(SVAL_TOOL_TABS, SVAL_POINT_SWITCH_INTERVAL_MS, 1, 320);
    assert(during == 1);
    during = flick(SVAL_TOOL_TABS, SVAL_POINT_SWITCH_INTERVAL_MS, 2, 330); // 660 counts
    assert(during == 2);
    configure(SVAL_TOOL_VOLUME, 0);
    during = flick(SVAL_TOOL_VOLUME, 0, 1, 320); // the same 320 counts: 5 Volume steps
    assert(during == 5);
    puts("catch-up: every discrete tool paced by its rate limit under hard flicks, nothing after the ball stops, Tabs / Tasks step and default pace passed");
}
