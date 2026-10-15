/*
Copyright 2026 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Svalboard debounce (DEBOUNCE_TYPE = custom, rules.mk SVAL_DEBOUNCE=optical).
 *
 * Every key but one is an optical interrupter: an LED and a phototransistor,
 * read once per frame while the row is lit. There is no contact to bounce, so
 * those keys pass straight through with no delay.
 *
 * The exception is the thumb Double Down key: a mechanical tact switch on the
 * thumb row (local row 0 on each half) in column DOUBLEDOWN_COL (COL6, the
 * column the finger clusters use for their optical Double South keys). Only
 * that one position is debounced, per key and deferred: a change is reported
 * once the raw level has held for SVAL_DD_DEBOUNCE_MS. A one-frame glitch,
 * such as the Double Down polarity check in kb/matrix.c at boot, never
 * becomes a key event.
 *
 * Each half debounces its own five rows (cooked = matrix + thisHand), so the
 * same local position covers both halves: left [0,5] and right [5,5].
 *
 * SVAL_DEBOUNCE=legacy restores QMK's default (sym_defer_g, 5 ms, all keys).
 */

#ifdef SVAL_DEBOUNCE_HOST_TEST
#    include "debounce_sval_host.h"
#else
#    include "debounce.h"
#    include "timer.h"
#    include "config.h"
#endif
#include <string.h>

#ifndef SVAL_DD_DEBOUNCE_MS
#    define SVAL_DD_DEBOUNCE_MS 5
#endif
#ifndef SVAL_DD_ROW
#    define SVAL_DD_ROW 0  // the thumb row, local to each half
#endif
#define SVAL_DD_BIT ((matrix_row_t)1 << DOUBLEDOWN_COL)

#if SVAL_DD_DEBOUNCE_MS > 250
#    error "SVAL_DD_DEBOUNCE_MS must be at most 250"
#endif

static bool     dd_settling;  // raw Double Down differs from cooked and is being timed
static uint16_t dd_since_ms;  // when raw last moved away from cooked

void debounce_init(void) {
    dd_settling = false;
    dd_since_ms = 0;
}

// One pass, with the time in ms. Split out of debounce() for the host tests.
bool sval_debounce_step(matrix_row_t raw[], matrix_row_t cooked[], uint16_t now_ms) {
    bool changed = false;

    // Optical keys: straight through.
    for (uint8_t r = 0; r < MATRIX_ROWS_PER_HAND; r++) {
        matrix_row_t keep = (r == SVAL_DD_ROW) ? SVAL_DD_BIT : 0;
        matrix_row_t next = (raw[r] & ~keep) | (cooked[r] & keep);
        if (next != cooked[r]) {
            cooked[r] = next;
            changed   = true;
        }
    }

    // Double Down: deferred, per key.
    bool raw_dd    = (raw[SVAL_DD_ROW] & SVAL_DD_BIT) != 0;
    bool cooked_dd = (cooked[SVAL_DD_ROW] & SVAL_DD_BIT) != 0;
    if (raw_dd == cooked_dd) {
        dd_settling = false;  // a bounce back restarts the wait
    } else {
        if (!dd_settling) {
            dd_settling = true;
            dd_since_ms = now_ms;
        }
        if ((uint16_t)(now_ms - dd_since_ms) >= SVAL_DD_DEBOUNCE_MS) {
            cooked[SVAL_DD_ROW] ^= SVAL_DD_BIT;
            dd_settling = false;
            changed     = true;
        }
    }
    return changed;
}

// Called on every matrix_scan(), changed or not, so the Double Down timer runs
// between frames too.
bool debounce(matrix_row_t raw[], matrix_row_t cooked[], bool changed) {
    (void)changed;
#ifdef SVAL_DEBOUNCE_HOST_TEST
    return sval_debounce_step(raw, cooked, host_now_ms());
#else
    return sval_debounce_step(raw, cooked, timer_read());
#endif
}
