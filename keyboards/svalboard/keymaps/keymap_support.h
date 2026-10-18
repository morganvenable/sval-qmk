/*
Copyright 2023 Morgan Venable @_claussen

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

#include QMK_KEYBOARD_H
#include <stdbool.h>
#include <stdint.h>
#include "svalboard.h"

// VIA's customKeycodes maps to QK_USER range (0x7E40+), not QK_KB (0x7E00+).
// TODO: Investigate using QK_KB range for keyboard-level keycodes in the future.
enum my_keycodes {
    SV_LEFT_DPI_INC = QK_USER_0,
    SV_LEFT_DPI_DEC,
    SV_RIGHT_DPI_INC,
    SV_RIGHT_DPI_DEC,
    SV_LEFT_SCROLL_TOGGLE,
    SV_RIGHT_SCROLL_TOGGLE,
    SV_RECALIBRATE_POINTER,
    SV_MH_CHANGE_TIMEOUTS,
    SV_CAPS_WORD,
    SV_AXIS_SCROLL_LOCK,
    SV_TOGGLE_23_67,
    SV_TOGGLE_45_67,
    SV_SNIPER_2,
    SV_SNIPER_3,
    SV_SNIPER_5,
    SV_SCROLL_HOLD,
    SV_SCROLL_TOGGLE,
    SV_OUTPUT_STATUS,
    SV_TOGGLE_AUTOMOUSE,
    SV_TURBO_SCAN,
    SV_NATURAL_SCROLL_TOGGLE,
    SV_SNIPER_2_TG,
    SV_SNIPER_3_TG,
    SV_SNIPER_5_TG,
    SV_BOOST_2,         // pos 24 — multiplies x/y AND h/v, like sniper divides both
    SV_BOOST_3,         // pos 25
    SV_BOOST_5,         // pos 26
    SV_BOOST_2_TG,      // pos 27
    SV_BOOST_3_TG,      // pos 28
    SV_BOOST_5_TG,      // pos 29
    SV_VELOCITY_SCROLL, // pos 30 — hold: pointer offset sets a scroll speed (velocity_scroll.h). The GUI cap is QK_USER_31 inclusive
    KC_NORMAL_HOLD = SAFE_RANGE,
    KC_FUNC_HOLD,
    SV_SAFE_RANGE, // Keycodes over this are safe on Svalboard.
};

#define MH_AUTO_BUTTONS_LAYER (DYNAMIC_KEYMAP_LAYER_COUNT - 1)

_Static_assert((int)SV_VELOCITY_SCROLL == (int)QK_USER_30, "sval.json customKeycodes position 30");

// Velocity Scroll is held (by its key or the touchpad dev command).
bool velocity_scroll_active(void);
// Dev builds (SVAL_TOUCH_DEV, raw HID 0xFC op 0x42 action 2): hold it with no
// key, for keyless test boards. Expires unless repeated within 500 ms.
void velocity_scroll_dev_hold(bool on);
