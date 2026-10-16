// SPDX-License-Identifier: GPL-2.0-or-later
#include "point_control.h"
#include "point_motion.h"
#include "point_config.h"
#include "point_output.h"
#include "point_proportional.h"
#include "usb_device_state.h"
#include "svalboard.h"
#include "print.h"
#include <string.h>

// A control is a tool slot, 1-4 (0 none). The keys are shared: one hold and one toggle for
// the whole board, and every pointer runs the control they resolve to. Each pointer keeps
// its own motion and inhibit state.
enum { NONE };
static bool is_tasks(uint8_t slot) {
    return slot && sval_point_tool(slot) == SVAL_TOOL_TASKS;
}
#define MOTION_SLOTS 24
// Only the newest held key counts: pressing another hold replaces it, and an older key that
// is still down never comes back.
static struct {
    uint8_t pos, control;
} hold;
static bool    consumed[MATRIX_ROWS * MATRIX_COLS];
static uint8_t toggled;
// The rate limit is the board's: one clock for every pointer, since they share the tool.
static uint32_t last_output;
static bool     emitted;
static struct {
    uint8_t control, prefer_axis, next_axis;
    bool    inhibited;
    struct {
        uint32_t count;
        int8_t   direction;
    } pending[2];
    uint16_t          cpi;
    int64_t           fraction[2];
    sval_point_turn_t turn[2];
    struct {
        int32_t  h, v;
        uint32_t time;
    } motion[MOTION_SLOTS];
    uint8_t motion_count;
} pointers[2];
static sval_proportional_t proportional[2];
static bool                is_proportional(uint8_t slot) {
    return slot && sval_point_tool(slot) == SVAL_TOOL_PROPORTIONAL;
}
static void reset_proportional(uint8_t side) {
    sval_proportional_reset(&proportional[side], timer_read32());
}
static uint8_t  allowed_axes = SVAL_AXIS_BOTH;
static bool     tracing, policy_enabled;
static uint8_t  next_side, session_side, output_side, output_axis;
static bool     output_owned;
static uint8_t  swallowed_buttons;
static uint32_t recognized, admitted, dropped;
static bool     host_mac;

void sval_point_host_mac(bool mac) {
    host_mac = mac;
}

static uint32_t pending_count(uint8_t side) {
    return pointers[side].pending[0].count + pointers[side].pending[1].count;
}
static void clear_pending(uint8_t side, uint8_t axis) {
    dropped += pointers[side].pending[axis].count;
    pointers[side].pending[axis].count = 0;
}
static void clear_motion(uint8_t side) {
    if (output_owned && output_side == side) sval_point_output_drop_unaccepted();
    memset(pointers[side].turn, 0, sizeof(pointers[side].turn));
    memset(pointers[side].fraction, 0, sizeof(pointers[side].fraction));
    clear_pending(side, 0);
    clear_pending(side, 1);
    pointers[side].motion_count = 0;
}
// The held key wins over the toggle. A pointer whose control changes ends what it was
// sending and starts the new tool from rest; one that keeps its control continues.
static void resolve(void) {
    uint8_t control = hold.control ? hold.control : toggled;
    for (uint8_t side = 0; side < 2; ++side) {
        if (control == pointers[side].control) continue;
        if (output_owned && output_side == side) {
            sval_point_output_end(false);
            output_owned = false;
        }
        pointers[side].control = control;
        reset_proportional(side);
        emitted                  = false;
        pointers[side].inhibited = false;
        clear_motion(side);
    }
}
static void stop_all(bool cancel) {
    sval_point_output_end(cancel);
    output_owned = false;
    hold.control = NONE;
    toggled      = NONE;
    emitted      = false;
    for (uint8_t side = 0; side < 2; ++side) {
        pointers[side].control = NONE;
        reset_proportional(side);
        pointers[side].inhibited = false;
        clear_motion(side);
    }
    // consumed[] survives until physical key-up; never leak a remapped release.
}
// After a task switcher closes, a held Tasks key stays inhibited (on every pointer) until
// it is released; a Tasks toggle turns off.
static void deactivate_tasks(void) {
    if (is_tasks(toggled)) toggled = NONE;
    for (uint8_t side = 0; side < 2; ++side) {
        if (is_tasks(pointers[side].control)) {
            pointers[side].inhibited = true;
            clear_motion(side);
        }
    }
    resolve();
}
static void end_tasks(bool cancel) {
    sval_point_output_end(cancel);
    output_owned = false;
    deactivate_tasks();
}
bool sval_point_active(uint8_t side) {
    return pointers[side].control != NONE;
}
bool sval_point_blocked(uint8_t side) {
    return sval_point_output_session() && side != session_side;
}

bool sval_point_process(uint16_t keycode, keyrecord_t *record) {
    bool point_key = keycode >= SV_POINT_FIRST && keycode <= SV_POINT_RESERVED_LAST;
    if (record->event.key.row >= MATRIX_ROWS || record->event.key.col >= MATRIX_COLS) {
        return !point_key;
    }
    uint8_t pos = record->event.key.row * MATRIX_COLS + record->event.key.col;
    if (!record->event.pressed && consumed[pos]) {
        consumed[pos] = false;
        // Releasing the newest hold ends it; releasing a key it replaced changes nothing.
        if (hold.control && hold.pos == pos) {
            hold.control = NONE;
            resolve();
        }
        return false;
    }
    if (point_key) {
        if (!record->event.pressed || consumed[pos]) return false;
        consumed[pos] = true;
        if (keycode <= SV_POINT_TOOL4_TOGGLE) {
            uint8_t n = keycode - SV_POINT_FIRST, control = n / 2 + 1;
            if (n & 1)
                toggled = toggled == control ? NONE : control;
            else {
                hold.pos     = pos;
                hold.control = control;
            }
            resolve();
            // A Proportional scroll press re-centers every pointer, even one that kept it.
            for (uint8_t side = 0; side < 2; ++side) {
                if (!is_proportional(pointers[side].control)) continue;
                clear_motion(side);
                reset_proportional(side);
            }
        } else if (keycode == SV_POINT_STOP_ALL)
            stop_all(true);
        else if (keycode == SV_POINT_TRACE) {
            tracing = !tracing;
            uprintf("point trace=%u recognized=%lu admitted=%lu dropped=%lu axes=%u\n", tracing, (unsigned long)recognized, (unsigned long)admitted, (unsigned long)dropped, allowed_axes);
        }
        // Every other code in the block is reserved or retired: consumed, nothing else.
        return false;
    }
    if (record->event.pressed && sval_point_output_session()) {
        if (keycode == KC_ESC || keycode == KC_ENT || (keycode >= QK_MOUSE_BUTTON_1 && keycode <= QK_MOUSE_BUTTON_8)) {
            consumed[pos] = true;
            end_tasks(keycode != KC_ENT && keycode != QK_MOUSE_BUTTON_1);
            return false;
        }
        // Prototype: commit before forwarding unrelated input. Report observer
        // also preempts macro/one-shot producers that bypass this physical hook.
        end_tasks(false);
    }
    return true;
}
void sval_point_buffer(uint8_t side, int16_t x, int16_t y, uint16_t cpi) {
    if (!sval_point_active(side) || pointers[side].inhibited) return;
    if (is_proportional(pointers[side].control)) {
        if (sval_point_output_session()) {
            reset_proportional(side);
            return;
        }
        if (pointers[side].cpi != cpi) {
            clear_motion(side);
            reset_proportional(side);
            pointers[side].cpi = cpi;
        }
        sval_proportional_move(&proportional[side], sval_point_normalize(x, cpi, &pointers[side].fraction[0]), -sval_point_normalize(y, cpi, &pointers[side].fraction[1]));
        return;
    }
    if (!x && !y) {
        clear_pending(side, 0);
        clear_pending(side, 1);
        if (output_owned && output_side == side) sval_point_output_drop_unaccepted();
        return;
    }
    if (sval_point_output_session() && side != session_side) {
        clear_motion(side);
        return;
    }
    if (pointers[side].cpi != cpi) {
        clear_motion(side);
        pointers[side].cpi = cpi;
    }
    uint8_t n = pointers[side].motion_count;
    if (n == MOTION_SLOTS) {
        ++dropped;
        return;
    }
    pointers[side].motion[n].h = sval_point_normalize(x, cpi, &pointers[side].fraction[0]);
    // Widen before negating the most negative sensor report.
    pointers[side].motion[n].v    = -sval_point_normalize(y, cpi, &pointers[side].fraction[1]);
    pointers[side].motion[n].time = timer_read32();
    ++pointers[side].motion_count;
}
void sval_point_discard_interval(void) {
    for (uint8_t side = 0; side < 2; ++side)
        sval_proportional_discard_output(&proportional[side], timer_read32());
    clear_pending(0, 0);
    clear_pending(0, 1);
    clear_pending(1, 0);
    clear_pending(1, 1);
    sval_point_output_drop_unaccepted();
    pointers[0].motion_count = pointers[1].motion_count = 0;
}
void sval_point_policy_changed(bool enabled) {
    if (policy_enabled == enabled) return;
    policy_enabled = enabled;
    clear_motion(0);
    clear_motion(1);
}
static void recognize(uint8_t side, uint8_t axis, int32_t delta, uint32_t now) {
    uint8_t control = pointers[side].control;
    uint8_t tool    = sval_point_tool(control);
    if (tool == SVAL_TOOL_PROPORTIONAL) return;
    if ((axis == 0 && (tool == SVAL_TOOL_VOLUME || tool == SVAL_TOOL_ZOOM)) || (axis == 1 && tool != SVAL_TOOL_VOLUME && tool != SVAL_TOOL_ZOOM && tool != SVAL_TOOL_ARROWS)) return;
    if (sval_point_inverted(control, axis)) delta = -delta;
    const sval_point_profile_t *profile  = sval_point_profile(control);
    int32_t                     counts   = sval_point_step_counts(tool, axis ? profile->vertical_distance : profile->distance);
    int32_t                     distance = counts * SVAL_POINT_ONE;
    // A new stroke after quantizer idle must not inherit the previous stroke's tail.
    if (pointers[side].turn[axis].moved && (uint32_t)(now - pointers[side].turn[axis].last_motion) >= 250) clear_pending(side, axis);
    sval_point_steps_t steps = sval_point_turn(&pointers[side].turn[axis], delta, distance, (profile->reversal < counts ? profile->reversal : counts) * SVAL_POINT_ONE, now);
    if (steps.reversed) {
        clear_pending(side, axis);
        if (output_owned && output_side == side && output_axis == axis) sval_point_output_drop_unaccepted();
    }
    if (!steps.steps) return;
    uint32_t count = steps.steps > 0 ? steps.steps : -steps.steps;
    recognized += count;
    int8_t direction = steps.steps > 0 ? 1 : -1;
    if (pointers[side].pending[axis].count && pointers[side].pending[axis].direction != direction) clear_pending(side, axis);
    pointers[side].pending[axis].count += count;
    pointers[side].pending[axis].direction = direction;
    if (tracing) uprintf("point side=%u control=%u axis=%u steps=%ld mask=%u\n", side, control, axis, (long)steps.steps, allowed_axes);
}
void sval_point_finish_interval(uint8_t axes) {
    // Current motion replaces previous-frame intent. Never replay excess motion later.
    clear_pending(0, 0);
    clear_pending(0, 1);
    clear_pending(1, 0);
    clear_pending(1, 1);
    sval_point_output_drop_unaccepted();
    if (axes != allowed_axes) {
        sval_point_output_drop_unaccepted();
        for (uint8_t side = 0; side < 2; ++side) {
            memset(pointers[side].turn, 0, sizeof(pointers[side].turn));
            memset(pointers[side].fraction, 0, sizeof(pointers[side].fraction));
            clear_pending(side, 0);
            clear_pending(side, 1);
        }
        allowed_axes = axes;
    }
    for (uint8_t side = 0; side < 2; ++side) {
        for (uint8_t i = 0; i < pointers[side].motion_count; ++i) {
            // Keep processing both free axes; output arbitration is round-robin.
            for (uint8_t k = 0; k < 2; ++k) {
                uint8_t axis = (k + pointers[side].prefer_axis) % 2;
                if (!(axes & (1 << axis))) continue;
                recognize(side, axis, axis ? pointers[side].motion[i].v : pointers[side].motion[i].h, pointers[side].motion[i].time);
            }
        }
        pointers[side].prefer_axis ^= 1;
        pointers[side].motion_count = 0;
    }
}
void sval_point_mouse(report_mouse_t *report) {
    swallowed_buttons &= report->buttons;
    report->buttons &= ~swallowed_buttons;
    if (!sval_point_output_session()) return;
    report->x = report->y = report->h = report->v = 0;
    if (report->buttons) {
        swallowed_buttons |= report->buttons;
        end_tasks((report->buttons & 1) == 0);
        report->buttons = 0;
    }
}
void sval_point_task(void) {
    if (!is_keyboard_master()) return;
    sval_point_transport_install();
    static bool was_connected;
    bool        connected = sval_other_half_connected();
    if (was_connected && !connected) stop_all(false);
    was_connected                      = connected;
    static usb_hid_protocol_t protocol = USB_PROTOCOL_REPORT;
    if (protocol != usb_device_state_get_protocol()) {
        stop_all(false);
        protocol = usb_device_state_get_protocol();
    }
    static bool suspended;
    bool        unavailable = usb_device_state_get_configure_state() != USB_DEVICE_STATE_CONFIGURED;
    if (unavailable && !suspended) stop_all(false);
    suspended = unavailable;
    if (unavailable) return;
    uint32_t now = timer_read32();
    // The held key wins over the toggle, so a held Tasks key owns any open session.
    sval_point_output_session_held(is_tasks(hold.control), now);
    sval_point_output_task(now);
    if (sval_point_output_interrupted()) {
        output_owned = false;
        deactivate_tasks();
    }
    for (uint8_t k = 0; k < 2; ++k) {
        uint8_t side = (next_side + k) % 2;
        if (!pending_count(side)) continue;
        if (sval_point_output_session() && side != session_side) {
            clear_motion(side);
            continue;
        }
        for (uint8_t axis = 0; axis < 2; ++axis) {
            if (pointers[side].pending[axis].count && pointers[side].inhibited) clear_pending(side, axis);
        }
        if (!pending_count(side)) continue;
        uint8_t axis = pointers[side].next_axis;
        if (!pointers[side].pending[axis].count) axis ^= 1;
        uint8_t  control  = pointers[side].control;
        uint16_t interval = sval_point_interval(control);
        // Over the rate limit: this interval's steps are dropped at its end, never queued.
        if (emitted && (uint32_t)(now - last_output) < interval) continue;
        bool                     positive = pointers[side].pending[axis].direction > 0;
        uint16_t                 keycode;
        uint8_t                  mods = 0;
        sval_point_output_kind_t kind = SVAL_POINT_KEY;
        uint8_t                  tool = sval_point_tool(control);
        if (tool == SVAL_TOOL_VOLUME) {
            kind    = SVAL_POINT_CONSUMER;
            keycode = positive ? 0xE9 : 0xEA;
        } else if (tool == SVAL_TOOL_ARROWS) {
            // Arrows go out with the user's held modifiers: Shift selects, Ctrl jumps words.
            keycode = axis ? (positive ? KC_UP : KC_DOWN) : (positive ? KC_RIGHT : KC_LEFT);
            kind    = SVAL_POINT_KEY_WITH_MODS;
        } else {
            // Windows shortcuts, or the Command ones when the host is macOS (US layout).
            uint8_t command = host_mac ? MOD_BIT(KC_LGUI) : MOD_BIT(KC_LCTL);
            if (tool == SVAL_TOOL_ZOOM) {
                // Ctrl+= / Ctrl+-, Command+= / Command+-.
                keycode = positive ? KC_EQUAL : KC_MINUS;
                mods    = command;
            } else if (tool == SVAL_TOOL_HISTORY || tool == SVAL_TOOL_HISTORY_Y) {
                // Undo Ctrl+Z; redo Ctrl+Shift+Z or Ctrl+Y. macOS: Command+Z, and both
                // redo with Command+Shift+Z, since many Mac apps ignore Command+Y.
                bool y  = positive && tool == SVAL_TOOL_HISTORY_Y && !host_mac;
                keycode = y ? KC_Y : KC_Z;
                mods    = command;
                if (positive && !y) mods |= MOD_BIT(KC_LSFT);
            } else if (tool == SVAL_TOOL_TABS && host_mac) {
                // Command+Shift+] / Command+Shift+[: next / previous tab.
                keycode = positive ? KC_RBRC : KC_LBRC;
                mods    = MOD_BIT(KC_LGUI) | MOD_BIT(KC_LSFT);
            } else {
                // Tabs Ctrl+Tab; Tasks Alt+Tab, or Command+Tab on macOS. Shift goes back.
                keycode = KC_TAB;
                mods    = tool == SVAL_TOOL_TABS ? MOD_BIT(KC_LCTL) : host_mac ? MOD_BIT(KC_LGUI) : MOD_BIT(KC_LALT);
                if (!positive) mods |= MOD_BIT(KC_LSFT);
                if (tool == SVAL_TOOL_TASKS) kind = SVAL_POINT_TASK;
            }
        }
        if (sval_point_output_start(kind, keycode, mods, now)) {
            output_side  = side;
            output_axis  = axis;
            output_owned = true;
            if (tool == SVAL_TOOL_TASKS) {
                session_side = side;
                clear_motion(side ^ 1);
                reset_proportional(side ^ 1);
            }
            --pointers[side].pending[axis].count;
            pointers[side].next_axis = axis ^ 1;
            last_output              = now;
            emitted                  = true;
            sval_point_output_task(now);
            ++admitted;
            next_side = side ^ 1;
            break;
        }
    }
}

void sval_point_stop(void) {
    stop_all(true);
}
void sval_point_tuning_changed(void) {
    clear_motion(0);
    clear_motion(1);
    reset_proportional(0);
    reset_proportional(1);
}
// STATUS format 1. Both pointers report the same tool; byte 8 is retired (the Finer /
// Coarser distance shift) and always reads 1, the normal step travel.
void sval_point_status(uint8_t *data) {
    data[2]             = 1;
    data[3]             = pointers[0].control;
    data[4]             = pointers[1].control;
    data[5]             = allowed_axes;
    data[6]             = policy_enabled;
    data[7]             = sval_point_output_session();
    data[8]             = 1;
    uint32_t counters[] = {recognized, admitted, dropped};
    for (uint8_t i = 0; i < 3; ++i)
        for (uint8_t b = 0; b < 4; ++b)
            data[9 + i * 4 + b] = counters[i] >> (b * 8);
}

// A displaced rate control is ongoing scroll intent, including when a sensor is idle.
bool sval_point_scroll_engaged(void) {
    for (uint8_t side = 0; side < 2; ++side)
        if (is_proportional(pointers[side].control) && sval_proportional_displaced(&proportional[side], sval_point_profile(pointers[side].control)->reversal - 1)) return true;
    return false;
}
void sval_point_scroll(report_mouse_t *report, uint8_t axes, bool natural, uint8_t host_divisor) {
    int32_t h = report->h, v = report->v;
    for (uint8_t side = 0; side < 2; ++side) {
        uint8_t slot = pointers[side].control;
        if (!is_proportional(slot)) continue;
        if (sval_point_output_session() || pointers[side].inhibited || usb_device_state_get_configure_state() != USB_DEVICE_STATE_CONFIGURED) {
            reset_proportional(side);
            continue;
        }
        const sval_point_profile_t *p = sval_point_profile(slot);
        int16_t                     out[2];
        uint8_t                     invert = (sval_point_inverted(slot, 0) ? 1 : 0) | (sval_point_inverted(slot, 1) ? 2 : 0);
#ifdef POINTING_DEVICE_HIRES_SCROLL_ENABLE
        const uint16_t units = POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER;
#else
        const uint16_t units = 1;
#endif
        sval_proportional_output(&proportional[side], p->distance, p->vertical_distance, p->reversal - 1, p->interval_ms, axes, invert, host_divisor, units, timer_read32(), out);
        h += out[0];
        v += natural ? -out[1] : out[1];
    }
    report->h = h > MOUSE_REPORT_HV_MAX ? MOUSE_REPORT_HV_MAX : h < MOUSE_REPORT_HV_MIN ? MOUSE_REPORT_HV_MIN : h;
    report->v = v > MOUSE_REPORT_HV_MAX ? MOUSE_REPORT_HV_MAX : v < MOUSE_REPORT_HV_MIN ? MOUSE_REPORT_HV_MIN : v;
}
