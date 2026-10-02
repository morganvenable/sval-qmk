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

#include "quantum.h"

extern const int16_t mh_timer_choices[6];

struct layer_hsv {
    uint8_t hue;
    uint8_t sat;
    uint8_t val;
};

struct __attribute__((__packed__)) saved_values {
    bool left_scroll :1;
    bool right_scroll :1;
    bool axis_scroll_lock: 1;
    bool auto_mouse: 1;
    bool natural_scroll: 1;
    bool left_automouse: 1;  // Left pointer movement activates the auto mouse layer
    bool right_automouse: 1; // Right pointer movement activates the auto mouse layer
    unsigned int unused0 :1;
    uint8_t left_dpi_index;
    uint8_t right_dpi_index;
    uint8_t mh_timer_index;
    struct layer_hsv layer_colors[DYNAMIC_KEYMAP_LAYER_COUNT];
    uint8_t turbo_scan;
    uint16_t automouse_threshold; // Movement distance required for layer activation (0=disabled)
    uint8_t automouse_decay;      // Accumulator decay time in 10ms units (0=no decay)
    uint16_t scan_prewait_us;     // Row-on settle time before reading (0 = use turbo table)
    uint16_t scan_postwait_us;    // Row-off recovery time after reading (0 = use turbo table)
};

// RPC structure for split keyboard sync
typedef struct __attribute__((__packed__)) _presence_rpc_t {
    uint8_t  turbo_scan;
    uint16_t scan_prewait_us;
    uint16_t scan_postwait_us;
} presence_rpc_t;

#define SVAL_TURBO_CHOICES 7

// Hardware revision, read once from SVAL_HW_REV_PIN (internal pull-up; the
// board straps the pin to ground on revision B "flipfet").
enum sval_hw_rev {
    SVAL_HW_REV_A       = 0,
    SVAL_HW_REV_FLIPFET = 1,
};
uint8_t sval_hw_rev(void);
bool    sval_hw_rev_is_flipfet(void);
bool    sval_other_half_connected(void);
void    sval_scan_timing(uint16_t *pre, uint16_t *post); // matrix.c
uint8_t sval_pushed_mask(bool thumbs);                    // matrix.c

typedef struct saved_values saved_values_t;

extern saved_values_t global_saved_values;
void output_keyboard_info(void);
void increase_left_dpi(void);
void increase_right_dpi(void);
void decrease_left_dpi(void);
void decrease_right_dpi(void);
void set_left_dpi(uint8_t index);
void set_right_dpi(uint8_t index);
void set_dpi_from_eeprom(void);
void write_eeprom_kb(void);
void read_eeprom_kb(void);
void change_turbo_scan(void);
void recalibrate_pointer(void);
void sval_set_active_layer(uint32_t layer, bool save);
void sval_on_reconnect(void);
int16_t get_left_dpi(void);
int16_t get_right_dpi(void);
