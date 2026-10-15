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
#pragma once

/*
 * Chatter log (dev builds only: -e SVAL_CHATTER_LOG=yes).
 *
 * Counts raw (pre-debounce) matrix transitions per key on this half, and the
 * "fast" ones: a press or a release shorter than any finger can make. That is
 * a transition less than SVAL_CHATTER_FAST_US after the key's previous one, or
 * one frame after it while frames are short (a one-frame pulse, which can be
 * longer than 3 ms when the main loop is slow). Read over raw HID through the Scan Lab channel (scanlab.h ops
 * SCANLAB_OP_CHATTER_*), for either half; tools/chatter.py is the host side.
 * Each fast transition is also printed on the console when it is enabled.
 */

#include <stdint.h>
#include <stdbool.h>

#ifndef SVAL_CHATTER_FAST_US
#    define SVAL_CHATTER_FAST_US 3000
#endif
#ifndef SVAL_CHATTER_ONE_FRAME_MAX_US
#    define SVAL_CHATTER_ONE_FRAME_MAX_US 15000  // a one-frame pulse counts while frames are shorter than this
#endif
#define SVAL_CHATTER_ROWS 5
#define SVAL_CHATTER_COLS 6

typedef struct {
    uint16_t transitions[SVAL_CHATTER_ROWS][SVAL_CHATTER_COLS];
    uint16_t fast[SVAL_CHATTER_ROWS][SVAL_CHATTER_COLS];
    uint16_t min_gap_us[SVAL_CHATTER_ROWS][SVAL_CHATTER_COLS];  // 0xFFFF: none under 65 ms seen
    uint32_t last_us[SVAL_CHATTER_ROWS][SVAL_CHATTER_COLS];
    uint32_t last_frame[SVAL_CHATTER_ROWS][SVAL_CHATTER_COLS];
    uint8_t  seen[SVAL_CHATTER_ROWS];  // bit c: the key has a previous transition
    uint32_t cleared_us;
} sval_chatter_t;

// One changed frame: old and new raw rows (this half's five), the frame's start
// time in us and its number (counting every scanned frame).
void sval_chatter_frame(const uint8_t *old_rows, const uint8_t *new_rows, uint32_t now_us, uint32_t frame);
void sval_chatter_clear(uint32_t now_us);
const sval_chatter_t *sval_chatter_stats(void);
