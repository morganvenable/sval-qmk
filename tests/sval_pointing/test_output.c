// Compiles the production output broker against an observable, fallible transport.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "point_output.h"
#include "keycodes.h"
static uint32_t clock_ms;
static bool blocked;
static report_keyboard_t seen_key;
static report_nkro_t seen_nkro;
static report_extra_t seen_extra;
static unsigned accepted;
uint32_t timer_read32(void) { return clock_ms; }
bool sval_point_transport_keyboard(report_keyboard_t *r) { if (blocked) return false; seen_key = *r; ++accepted; return true; }
bool sval_point_transport_nkro(report_nkro_t *r) { if (blocked) return false; seen_nkro = *r; ++accepted; return true; }
bool sval_point_transport_extra(report_extra_t *r) { if (blocked) return false; seen_extra = *r; ++accepted; return true; }
static void tick(unsigned ms) { clock_ms += ms; sval_point_output_task(clock_ms); }
static void settle(void) { tick(1); tick(21); tick(1); tick(21); }
int main(void) {
    report_keyboard_t empty = {0};
    sval_point_output_observe_keyboard(&empty);
    assert(sval_point_output_start(SVAL_POINT_TASK, KC_TAB, 4, clock_ms));
    tick(1); assert(seen_key.mods == 4 && seen_key.keys[0] == KC_TAB);
    settle(); assert(seen_key.mods == 4 && seen_key.keys[0] == 0);
    // Reverse retains Alt, applies Shift only to its Tab pulse.
    assert(sval_point_output_start(SVAL_POINT_TASK, KC_TAB, 6, clock_ms));
    tick(1); assert(seen_key.mods == 6);
    settle(); assert(seen_key.mods == 4);
    // A physical/macro report preempts synthetic mods without modifying its input.
    report_keyboard_t physical = {.mods = 2, .keys = {4}};
    sval_point_output_observe_keyboard(&physical);
    assert(!sval_point_output_session());
    assert(seen_key.mods == 2 && seen_key.keys[0] == 4 && physical.mods == 2);
    assert(sval_point_output_interrupted());
    settle();
    assert(!sval_point_output_start(SVAL_POINT_KEY, KC_TAB, 1, clock_ms));
    sval_point_output_observe_keyboard(&empty); settle();
    // Failed down is retried; never start hold duration until acceptance.
    blocked = true;
    unsigned count = accepted;
    assert(sval_point_output_start(SVAL_POINT_KEY, KC_TAB, 1, clock_ms));
    tick(30); assert(accepted == count);
    blocked = false; tick(1); assert(seen_key.keys[0] == KC_TAB);
    blocked = true; tick(21); assert(seen_key.keys[0] == KC_TAB);
    tick(50); assert(seen_key.keys[0] == KC_TAB);
    blocked = false; tick(1); assert(seen_key.keys[0] == 0 && seen_key.mods == 0);
    settle();
    // Stale unaccepted down never gets emitted after the transport recovers.
    blocked = true; assert(sval_point_output_start(SVAL_POINT_KEY, KC_TAB, 1, clock_ms));
    tick(81); blocked = false; settle(); assert(seen_key.keys[0] == 0);
    // A policy/gain/generation change can invalidate an unaccepted press.
    blocked = true; assert(sval_point_output_start(SVAL_POINT_KEY, KC_TAB, 1, clock_ms));
    sval_point_output_drop_unaccepted(); blocked = false; settle();
    assert(seen_key.keys[0] == 0 && seen_key.mods == 0);
    // Consumer collision leaves physical media usage held.
    assert(sval_point_output_start(SVAL_POINT_CONSUMER, 0xE9, 0, clock_ms));
    tick(1); assert(seen_extra.usage == 0xE9);
    report_extra_t physical_media = {.report_id = REPORT_ID_CONSUMER, .usage = 0xEA};
    sval_point_output_observe_extra(&physical_media); settle(); assert(seen_extra.usage == 0xEA);
    assert(!sval_point_output_start(SVAL_POINT_CONSUMER, 0xE9, 0, clock_ms));
    physical_media.usage = 0; sval_point_output_observe_extra(&physical_media); settle();
    // NKRO uses an independent copy and clears only synthetic contribution.
    report_nkro_t nkro = {.report_id = REPORT_ID_NKRO};
    sval_point_output_observe_nkro(&nkro); settle();
    assert(sval_point_output_start(SVAL_POINT_TASK, KC_TAB, 4, clock_ms));
    tick(1); assert(seen_nkro.mods == 4 && (seen_nkro.bits[KC_TAB / 8] & (1 << (KC_TAB % 8))));
    sval_point_output_end(true); assert(seen_nkro.mods == 4 && (seen_nkro.bits[KC_ESC / 8] & (1 << (KC_ESC % 8))));
    settle(); assert(seen_nkro.mods == 0 && !seen_nkro.bits[KC_ESC / 8]);
    assert(!nkro.mods && !nkro.bits[KC_TAB / 8]);
    // Watchdog also progresses with no motion callbacks.
    assert(sval_point_output_start(SVAL_POINT_TASK, KC_TAB, 4, clock_ms));
    tick(1); settle(); tick(10000); settle(); assert(!sval_point_output_session() && !seen_nkro.mods);
    // A failed release survives switching from NKRO to a 6KRO base report.
    assert(sval_point_output_start(SVAL_POINT_TASK, KC_TAB, 4, clock_ms));
    tick(1); assert(seen_nkro.mods == 4);
    blocked = true; sval_point_output_observe_keyboard(&physical);
    blocked = false; tick(1);
    assert(seen_nkro.mods == 0 && !seen_nkro.bits[KC_TAB / 8]);
    assert(seen_key.mods == 2 && seen_key.keys[0] == 4);
    puts("output broker: ownership, collisions, acceptance, release retry, expiry, NKRO, cancellation, watchdog passed");
}
