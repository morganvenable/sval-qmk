// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Updater LED states (D24), shown on this half's WS2812 at SVAL_UPDATE_LED_VAL.
// While any of them is shown, rgblight's own output is held off (D20, see
// update_led.c); UPDATE_LED_NONE hands the LEDs back.

typedef enum {
    UPDATE_LED_NONE = 0, // rgblight as usual
    UPDATE_LED_CONFIRM,  // blue blink: make the chord
    UPDATE_LED_CHORD,    // bright white double flash: chord recognised
    UPDATE_LED_APPROVED, // solid blue: approved, waiting for the host to begin
    UPDATE_LED_PROGRESS, // rainbow: verifying, erasing, receiving
    UPDATE_LED_WRITING,  // solid magenta: committing
    UPDATE_LED_ERROR,    // red, 1 Hz, 50/50
} update_led_mode_t;

// Called on every updater_task() pass with the mode for the current state.
void update_led_show(update_led_mode_t mode);
