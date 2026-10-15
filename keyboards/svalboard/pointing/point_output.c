// SPDX-License-Identifier: GPL-2.0-or-later
#include "point_output.h"
#include "timer.h"
#include "keycodes.h"
#include <string.h>

static report_keyboard_t base_keyboard;
static report_nkro_t     base_nkro;
static uint16_t          base_consumer;
static bool              use_nkro, keyboard_dirty, consumer_dirty, session, interrupted;
static bool              retired_keyboard, retired_nkro;
static uint8_t           key, mods, session_mod;
static uint16_t          consumer;
static enum { IDLE, DOWN, HELD, UP, GAP, CANCEL_DOWN, CANCEL_HELD } phase;
static uint32_t phase_time, session_time;

static bool base_busy(void) {
    if (use_nkro) {
        if (base_nkro.mods) return true;
        for (unsigned i = 0; i < sizeof(base_nkro.bits); ++i)
            if (base_nkro.bits[i]) return true;
    } else {
        if (base_keyboard.mods) return true;
        for (unsigned i = 0; i < sizeof(base_keyboard.keys); ++i)
            if (base_keyboard.keys[i]) return true;
    }
    return false;
}
static void flush(void) {
    // Format changes must not lose a failed release on the previous report ID.
    if (retired_keyboard) {
        report_keyboard_t neutral = {0};
#ifdef KEYBOARD_SHARED_EP
        neutral.report_id = REPORT_ID_KEYBOARD;
#endif
        if (!sval_point_transport_keyboard(&neutral)) return;
        retired_keyboard = false;
    }
    if (retired_nkro) {
        report_nkro_t neutral = {.report_id = REPORT_ID_NKRO};
        if (!sval_point_transport_nkro(&neutral)) return;
        retired_nkro = false;
    }
    if (keyboard_dirty) {
        if (use_nkro) {
            report_nkro_t r = base_nkro;
            r.report_id     = REPORT_ID_NKRO;
            r.mods |= mods;
            if (key && key / 8 < sizeof(r.bits)) r.bits[key / 8] |= 1 << (key % 8);
            keyboard_dirty = !sval_point_transport_nkro(&r);
        } else {
            report_keyboard_t r = base_keyboard;
#ifdef KEYBOARD_SHARED_EP
            r.report_id = REPORT_ID_KEYBOARD;
#endif
            r.mods |= mods;
            if (key) {
                for (unsigned i = 0; i < sizeof(r.keys); ++i) {
                    if (!r.keys[i]) {
                        r.keys[i] = key;
                        break;
                    }
                }
            }
            keyboard_dirty = !sval_point_transport_keyboard(&r);
        }
    }
    if (consumer_dirty) {
        report_extra_t r = {.report_id = REPORT_ID_CONSUMER, .usage = base_consumer ? base_consumer : consumer};
        consumer_dirty   = !sval_point_transport_extra(&r);
    }
}
void sval_point_output_end(bool cancel) {
    bool was_session = session;
    session          = false;
    if (cancel && was_session && !base_busy()) {
        // Escape while the session modifier is still owned, then release both.
        key            = KC_ESC;
        mods           = session_mod;
        keyboard_dirty = true;
        phase          = CANCEL_DOWN;
    } else {
        if (key || mods) keyboard_dirty = true;
        if (consumer) consumer_dirty = true;
        key = mods = 0;
        consumer   = 0;
        phase      = UP;
    }
    phase_time = timer_read32();
    flush();
}
void sval_point_output_drop_unaccepted(void) {
    if (phase == DOWN && (keyboard_dirty || consumer_dirty)) {
        if (session) interrupted = true;
        sval_point_output_end(false);
    }
}
static void preempt(void) {
    if (session) interrupted = true;
    sval_point_output_end(false);
}
void sval_point_output_observe_keyboard(const report_keyboard_t *report) {
    // Cancel before composition, so no physical/macro key inherits synthetic mods.
    bool changed = use_nkro || memcmp(&base_keyboard, report, sizeof(*report));
    if (changed && (key || mods || session)) preempt();
    if (use_nkro) retired_nkro = true;
    base_keyboard  = *report;
    use_nkro       = false;
    keyboard_dirty = true;
    flush();
}
void sval_point_output_observe_nkro(const report_nkro_t *report) {
    bool changed = !use_nkro || memcmp(&base_nkro, report, sizeof(*report));
    if (changed && (key || mods || session)) preempt();
    if (!use_nkro) retired_keyboard = true;
    base_nkro      = *report;
    use_nkro       = true;
    keyboard_dirty = true;
    flush();
}
void sval_point_output_observe_extra(const report_extra_t *report) {
    if (report->report_id != REPORT_ID_CONSUMER) {
        sval_point_transport_extra((report_extra_t *)report);
        return;
    }
    if (consumer || session) preempt();
    base_consumer  = report->usage;
    consumer_dirty = true;
    flush();
}
bool sval_point_output_start(sval_point_output_kind_t kind, uint16_t usage, uint8_t requested_mods, uint32_t now) {
    if (phase != IDLE || keyboard_dirty || consumer_dirty || retired_keyboard || retired_nkro || base_busy() || base_consumer) return false;
    if (session && kind != SVAL_POINT_TASK) return false;
    if (kind == SVAL_POINT_CONSUMER) {
        consumer       = usage;
        consumer_dirty = true;
    } else {
        if (usage > 255) return false;
        key            = usage;
        mods           = requested_mods;
        keyboard_dirty = true;
        if (kind == SVAL_POINT_TASK) {
            session      = true;
            session_mod  = requested_mods & ~((1 << 1) | (1 << 5)); // remove Shift
            session_time = now;
        }
    }
    phase      = DOWN;
    phase_time = now;
    return true;
}
void sval_point_output_task(uint32_t now) {
    if (session && (uint32_t)(now - session_time) >= 10000) {
        interrupted = true;
        sval_point_output_end(true);
    }
    // An unaccepted press cannot hang around until the transport recovers.
    if ((phase == DOWN || phase == CANCEL_DOWN) && (uint32_t)(now - phase_time) >= 80 && (keyboard_dirty || consumer_dirty)) {
        if (session) interrupted = true;
        sval_point_output_end(false);
    }
    flush();
    if (keyboard_dirty || consumer_dirty || retired_keyboard || retired_nkro) return;
    switch (phase) {
        case DOWN:
            phase      = HELD;
            phase_time = now;
            break;
        case CANCEL_DOWN:
            phase      = CANCEL_HELD;
            phase_time = now;
            break;
        case CANCEL_HELD:
        case HELD: { // Ordered accepted USB reports pace the pulse; no hidden dwell time.
            if (key || mods) keyboard_dirty = true;
            if (consumer) consumer_dirty = true;
            key      = 0;
            mods     = session ? session_mod : 0;
            consumer = 0;
            phase    = UP;
            flush();
        } break;
        case UP:
            phase      = GAP;
            phase_time = now;
            break;
        case GAP:
            phase = IDLE;
            break;
        default:
            break;
    }
}
bool sval_point_output_session(void) {
    return session;
}
bool sval_point_output_interrupted(void) {
    bool result = interrupted;
    interrupted = false;
    return result;
}
void sval_point_output_reset(void) {
    // Preserve base reports; release only this producer, including after suspend.
    interrupted = false;
    sval_point_output_end(false);
}
