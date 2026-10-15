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
#include "chatter.h"
#include <string.h>
#if defined(CONSOLE_ENABLE) && !defined(SVAL_DEBOUNCE_HOST_TEST)
#    include "print.h"
#endif

static sval_chatter_t st;

void sval_chatter_clear(uint32_t now_us) {
    memset(&st, 0, sizeof(st));
    memset(st.min_gap_us, 0xFF, sizeof(st.min_gap_us));
    st.cleared_us = now_us;
}

const sval_chatter_t *sval_chatter_stats(void) {
    return &st;
}

void sval_chatter_frame(const uint8_t *old_rows, const uint8_t *new_rows, uint32_t now_us) {
    static bool init = false;
    if (!init) {
        sval_chatter_clear(now_us);
        init = true;
    }
    for (uint8_t r = 0; r < SVAL_CHATTER_ROWS; r++) {
        uint8_t d = old_rows[r] ^ new_rows[r];
        for (uint8_t c = 0; d && c < SVAL_CHATTER_COLS; c++) {
            uint8_t bit = (uint8_t)(1u << c);
            if (!(d & bit)) continue;
            d &= (uint8_t)~bit;
            if (st.transitions[r][c] < 0xFFFF) st.transitions[r][c]++;
            if (st.seen[r] & bit) {
                uint32_t gap = now_us - st.last_us[r][c];
                if (gap < st.min_gap_us[r][c]) st.min_gap_us[r][c] = (uint16_t)gap;
                if (gap < SVAL_CHATTER_FAST_US) {
                    if (st.fast[r][c] < 0xFFFF) st.fast[r][c]++;
#if defined(CONSOLE_ENABLE) && !defined(SVAL_DEBOUNCE_HOST_TEST)
                    uprintf("chatter r%u c%u %s after %lu us\n", r, c, (new_rows[r] & bit) ? "down" : "up", (unsigned long)gap);
#endif
                }
            }
            st.seen[r] |= bit;
            st.last_us[r][c] = now_us;
        }
    }
}
